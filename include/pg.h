#ifndef PG_H_
#define PG_H_

#include "db_error.h"
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <libpq-fe.h>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------
// pg
//
// A thin RAII layer over libpq, sized to exactly what the PostgreSQL consumer
// and the schema migrator need: an owning connection, an explicit-transaction
// guard, an owning result, and a text-format parameter builder. It exists so
// those two translation units can talk to libpq without hand-rolling PQclear /
// PQfinish lifetimes or the const char* const* parameter marshalling, and so
// the rest of the project keeps depending only on the stable C client library
// rather than a C++-ABI-coupled wrapper.
//
// Error model: every fallible call returns std::expected<_, DbError>, matching
// the rest of the bridge. The DbError is built inside the shim from the failed
// result's SQLSTATE (PG_DIAG_SQLSTATE) and message, so callers just propagate
// it. A dropped connection is detected here (PQstatus == CONNECTION_BAD) and
// always classified DbError::Kind::PROTOCOL, regardless of the errKind the
// caller asked for, so the worker's reconnect path fires on a dropped link. The
// errKind argument only labels genuine query/migration failures; see
// DbError::deduceSeverity for how (kind, sqlstate) maps to TRANSIENT vs FATAL.
//
// Threading: none of these types is synchronized. They are used solely on the
// PostgresClient worker thread, which owns its connection.
// ---------------------------------------------------------------------------

namespace pg {

// ---------------------------------------------------------------------------
// Result
//
// Owning wrapper around PGresult. Move-only; clears on destruction. Accessors
// read text-format cells (the only result format the shim issues). value()
// returns the libpq-owned, NUL-terminated buffer, valid until this Result is
// destroyed.
// ---------------------------------------------------------------------------

class Result {
public:
  Result() = default;
  explicit Result(PGresult *res) noexcept : res_(res) {}
  ~Result() { clear(); }

  Result(Result &&other) noexcept : res_(other.res_) { other.res_ = nullptr; }
  Result &operator=(Result &&other) noexcept {
    if (this != &other) {
      clear();
      res_ = other.res_;
      other.res_ = nullptr;
    }
    return *this;
  }

  Result(const Result &) = delete;
  Result &operator=(const Result &) = delete;

  int rows() const noexcept { return res_ ? PQntuples(res_) : 0; }
  bool empty() const noexcept { return rows() == 0; }
  bool isNull(int row, int col) const noexcept {
    return PQgetisnull(res_, row, col) != 0;
  }
  // Text value of a cell. Empty string for a SQL NULL (guard with isNull()
  // first when the distinction matters).
  const char *value(int row, int col) const noexcept {
    return PQgetvalue(res_, row, col);
  }
  // Parse a cell as int. Used for the schema-version ledger, whose values are
  // produced by COALESCE(MAX(version), 0) and are always well-formed integers;
  // returns 0 on the impossible parse failure rather than throwing.
  int asInt(int row, int col) const noexcept;

  PGresult *get() const noexcept { return res_; }

private:
  void clear() noexcept {
    if (res_)
      PQclear(res_);
    res_ = nullptr;
  }

  PGresult *res_{nullptr};
};

// ---------------------------------------------------------------------------
// Params
//
// Builds the parameter arrays for PQexecParams in text format. Every value is
// rendered to a string up front; a SQL NULL is a std::nullopt slot, surfaced
// to libpq as a null pointer. Pointers into the stored strings are materialized
// only by values(), after the builder is fully populated, so vector growth
// during construction can never dangle them.
//
// Booleans render as "t"/"f"; non-finite floats render as the canonical
// PostgreSQL spellings "NaN"/"Infinity"/"-Infinity"; finite floats use the
// shortest round-trippable form. std::optional<T> binds *value or NULL.
// ---------------------------------------------------------------------------

class Params {
public:
  Params() = default;

  // Build from a heterogeneous list of bind values, dispatching each to the
  // matching add() overload. Constrained so a single Params argument cannot be
  // captured here (which would shadow copy-construction): such a call falls
  // through to the implicitly declared copy constructor instead.
  template <typename... Ts>
    requires(sizeof...(Ts) != 1 ||
             (!std::same_as<std::remove_cvref_t<Ts>, Params> && ...))
  explicit Params(Ts &&...values) {
    cells_.reserve(sizeof...(Ts));
    (add(std::forward<Ts>(values)), ...);
  }

  Params &add(std::nullptr_t) {
    cells_.emplace_back(std::nullopt);
    return *this;
  }
  Params &add(std::string value) {
    cells_.emplace_back(std::move(value));
    return *this;
  }
  Params &add(std::string_view value) {
    cells_.emplace_back(std::string{value});
    return *this;
  }
  Params &add(const char *value) {
    cells_.emplace_back(std::string{value});
    return *this;
  }
  Params &add(bool value) {
    cells_.emplace_back(value ? "t" : "f");
    return *this;
  }
  Params &add(short value) { return addInteger(value); }
  Params &add(int value) { return addInteger(value); }
  Params &add(long value) { return addInteger(value); }
  Params &add(long long value) { return addInteger(value); }
  Params &add(float value) { return addFloat(value); }
  Params &add(double value) { return addFloat(value); }

  template <typename T> Params &add(const std::optional<T> &value) {
    if (value)
      return add(*value);
    return add(nullptr);
  }

  // Number of bound parameters (the nParams argument to PQexecParams).
  int count() const noexcept { return static_cast<int>(cells_.size()); }

  // The paramValues array for PQexecParams: one pointer per parameter, null
  // where the parameter is SQL NULL. Rebuilt on each call from the (now stable)
  // cell storage; the returned pointer is valid until the next values() call or
  // until this Params is destroyed.
  const char *const *values() const;

private:
  template <typename T> Params &addInteger(T value) {
    cells_.emplace_back(std::to_string(value));
    return *this;
  }
  template <typename T> Params &addFloat(T value);

  std::vector<std::optional<std::string>> cells_;
  mutable std::vector<const char *> ptrs_;
};

// ---------------------------------------------------------------------------
// Conn
//
// Owning connection (PQfinish on destruction). Construction issues PQconnectdb
// and never throws; the caller checks isOpen() and, on failure, reads
// connectError() to obtain a CONNECT-kind DbError. Held by PostgresClient as a
// std::unique_ptr<Conn> so the producing header can forward-declare it and stay
// free of <libpq-fe.h>.
//
// Statements run directly on the connection: exec() wraps PQexec (no
// parameters, accepts a multi-statement body, e.g. a migration file) and
// execParams() wraps PQexecParams (text-format parameters, single statement).
// Both return the result or a DbError built from the server SQLSTATE; a dropped
// link is reported as PROTOCOL. There is no transaction object in libpq: a
// transaction is a BEGIN/COMMIT bracket on the connection, expressed by the
// Transaction guard below, and execs issued while it is alive run inside it.
// ---------------------------------------------------------------------------

class Conn {
public:
  explicit Conn(const std::string &dsn) noexcept
      : conn_(PQconnectdb(dsn.c_str())) {}
  ~Conn() { close(); }

  Conn(Conn &&other) noexcept : conn_(other.conn_) { other.conn_ = nullptr; }
  Conn &operator=(Conn &&other) noexcept {
    if (this != &other) {
      close();
      conn_ = other.conn_;
      other.conn_ = nullptr;
    }
    return *this;
  }

  Conn(const Conn &) = delete;
  Conn &operator=(const Conn &) = delete;

  bool isOpen() const noexcept {
    return conn_ && PQstatus(conn_) == CONNECTION_OK;
  }

  // CONNECT-kind DbError describing why the connection is not open. Valid to
  // call whether PQconnectdb returned a bad connection or (on OOM) nullptr.
  DbError connectError() const;

  // Run a statement with no parameters via PQexec. Accepts a multi-statement
  // body (used for the migration SQL files and for BEGIN/COMMIT). On failure
  // returns a DbError tagged with errKind, or PROTOCOL if the link dropped.
  std::expected<Result, DbError>
  exec(std::string_view sql, DbError::Kind errKind = DbError::Kind::QUERY);

  // Run a single parameterized statement via PQexecParams in text format. See
  // Params for how values are marshalled; errKind handling matches exec().
  std::expected<Result, DbError>
  execParams(std::string_view sql, const Params &params,
             DbError::Kind errKind = DbError::Kind::QUERY);

  // Quote an SQL identifier (schema name) for safe interpolation into DDL, via
  // PQescapeIdentifier. Returns the quoted identifier including the surrounding
  // double quotes. An allocation failure yields an empty string, which surfaces
  // downstream as a SQL syntax error rather than silent corruption.
  std::string quoteName(std::string_view name) const;

  // Attach libpq's wire-level protocol trace to an already-open FILE*. No-op if
  // the connection or file is null.
  void trace(std::FILE *stream) noexcept {
    if (conn_ && stream)
      PQtrace(conn_, stream);
  }

  // Install a libpq notice receiver (PQsetNoticeReceiver) so server
  // NOTICE/WARNING messages can be routed through the application's logging
  // rather than libpq's default sink (a raw write to stderr). Re-applied per
  // connection: a fresh PGconn starts with the default receiver.
  void setNoticeReceiver(PQnoticeReceiver receiver, void *arg) noexcept {
    if (conn_)
      PQsetNoticeReceiver(conn_, receiver, arg);
  }

  void close() noexcept {
    if (conn_)
      PQfinish(conn_);
    conn_ = nullptr;
  }

  PGconn *get() const noexcept { return conn_; }

private:
  PGconn *conn_{nullptr};
};

// ---------------------------------------------------------------------------
// Transaction
//
// Scope guard for an explicit transaction. begin() issues BEGIN on the
// connection and returns the guard, or a DbError if even BEGIN fails (e.g. the
// link is already down). It carries no exec of its own: statements run through
// Conn::exec/execParams on the same connection and participate in the open
// transaction. commit() issues COMMIT. On destruction the guard issues a
// ROLLBACK iff the connection is still in a transaction block
// (PQtransactionStatus reports PQTRANS_INTRANS or PQTRANS_INERROR), so an early
// return on any statement error unwinds the partial transaction while a
// successful commit() or a dropped link leaves nothing to roll back -- no
// bookkeeping flag required.
//
// Wrap only genuinely multi-statement units; a lone statement is already atomic
// under autocommit and needs no Transaction.
//
// Move-only and move-constructible (so begin() can return it by value); the
// moved-from guard is inert.
// ---------------------------------------------------------------------------

class Transaction {
public:
  static std::expected<Transaction, DbError>
  begin(Conn &conn, DbError::Kind errKind = DbError::Kind::QUERY);

  ~Transaction();

  Transaction(Transaction &&other) noexcept : conn_(other.conn_) {
    other.conn_ = nullptr;
  }
  Transaction &operator=(Transaction &&) = delete;
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;

  std::expected<void, DbError>
  commit(DbError::Kind errKind = DbError::Kind::QUERY);

private:
  explicit Transaction(Conn &conn) noexcept : conn_(&conn) {}

  Conn *conn_{nullptr};
};

} // namespace pg

#endif /* PG_H_ */
