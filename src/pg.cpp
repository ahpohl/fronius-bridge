#include "pg.h"
#include "db_error.h"
#include <charconv>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <libpq-fe.h>
#include <string>
#include <string_view>
#include <utility>

namespace pg {

namespace {

// A command (INSERT/DDL/SET/COMMIT) reports PGRES_COMMAND_OK; a row-returning
// query (SELECT) reports PGRES_TUPLES_OK. Everything else - including a null
// result from PQexec/PQexecParams on a fatal error - is a failure.
bool resultOk(PGresult *res) noexcept {
  if (!res)
    return false;
  const ExecStatusType status = PQresultStatus(res);
  return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
}

// Build a DbError from a failed exec. A dropped link wins unconditionally and
// is reported as PROTOCOL, the worker's reconnect trigger; otherwise the
// caller's errKind is paired with the server SQLSTATE so
// DbError::deduceSeverity can classify it. The message is trimmed to its first
// line to keep multi-line server context out of single-line log entries.
DbError fromFailedResult(PGconn *conn, PGresult *res, DbError::Kind errKind) {
  if (PQstatus(conn) == CONNECTION_BAD)
    return DbError::make(DbError::Kind::PROTOCOL, "{}",
                         DbError::firstLine(PQerrorMessage(conn)));

  const char *sqlstate =
      res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : nullptr;
  const char *message = res ? PQresultErrorMessage(res) : PQerrorMessage(conn);
  const std::string_view view = message ? message : "";

  if (sqlstate)
    return DbError::makeWithState(errKind, sqlstate, "{}",
                                  DbError::firstLine(view));
  return DbError::make(errKind, "{}", DbError::firstLine(view));
}

} // namespace

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

int Result::asInt(int row, int col) const noexcept {
  const char *text = PQgetvalue(res_, row, col);
  if (!text)
    return 0;
  const std::string_view view{text};
  int out = 0;
  std::from_chars(view.data(), view.data() + view.size(), out);
  return out;
}

// ---------------------------------------------------------------------------
// Params
// ---------------------------------------------------------------------------

template <typename T> Params &Params::addFloat(T value) {
  if (std::isnan(value)) {
    cells_.emplace_back("NaN");
  } else if (std::isinf(value)) {
    cells_.emplace_back(value < 0 ? "-Infinity" : "Infinity");
  } else {
    cells_.emplace_back(std::format("{}", value));
  }
  return *this;
}

template Params &Params::addFloat<float>(float);
template Params &Params::addFloat<double>(double);

const char *const *Params::values() const {
  ptrs_.clear();
  ptrs_.reserve(cells_.size());
  for (const auto &cell : cells_)
    ptrs_.push_back(cell ? cell->c_str() : nullptr);
  return ptrs_.data();
}

// ---------------------------------------------------------------------------
// Conn
// ---------------------------------------------------------------------------

DbError Conn::connectError() const {
  const char *message =
      conn_ ? PQerrorMessage(conn_) : "failed to allocate libpq connection";
  return DbError::make(DbError::Kind::CONNECT, "{}",
                       DbError::firstLine(message ? message : ""));
}

std::string Conn::quoteName(std::string_view name) const {
  char *quoted = PQescapeIdentifier(conn_, name.data(), name.size());
  std::string out{quoted ? quoted : ""};
  if (quoted)
    PQfreemem(quoted);
  return out;
}

std::expected<Result, DbError> Conn::exec(std::string_view sql,
                                          DbError::Kind errKind) {
  // PQexec needs a NUL-terminated string and accepts a multi-statement body
  // (the migration files use this). string_view is not guaranteed terminated,
  // so materialize it.
  const std::string query{sql};
  Result res{PQexec(conn_, query.c_str())};
  if (!resultOk(res.get()))
    return std::unexpected(fromFailedResult(conn_, res.get(), errKind));
  return res;
}

std::expected<Result, DbError> Conn::execParams(std::string_view sql,
                                                const Params &params,
                                                DbError::Kind errKind) {
  const std::string query{sql};
  // nullptr paramTypes -> server infers each parameter's type from its use site
  // (all our parameters land in typed INSERT columns or known function args);
  // nullptr paramLengths/paramFormats -> text format throughout; final 0 ->
  // text-format results.
  Result res{PQexecParams(conn_, query.c_str(), params.count(), nullptr,
                          params.values(), nullptr, nullptr, 0)};
  if (!resultOk(res.get()))
    return std::unexpected(fromFailedResult(conn_, res.get(), errKind));
  return res;
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

std::expected<Transaction, DbError> Transaction::begin(Conn &conn,
                                                       DbError::Kind errKind) {
  if (auto res = conn.exec("BEGIN", errKind); !res)
    return std::unexpected(std::move(res).error());
  return Transaction{conn};
}

Transaction::~Transaction() {
  // Roll back iff the connection is still inside a transaction block. A
  // successful commit() leaves it idle and a dropped link leaves it unknown, so
  // neither rolls back; an early return after a failed statement leaves it
  // in-transaction (or in-error) and is unwound here. The status is the single
  // source of truth, so no separate "committed" flag is needed. Best-effort:
  // the result is ignored.
  if (!conn_)
    return;
  const PGTransactionStatusType status = PQtransactionStatus(conn_->get());
  if (status == PQTRANS_INTRANS || status == PQTRANS_INERROR) {
    PGresult *res = PQexec(conn_->get(), "ROLLBACK");
    PQclear(res);
  }
}

std::expected<void, DbError> Transaction::commit(DbError::Kind errKind) {
  if (auto res = conn_->exec("COMMIT", errKind); !res)
    return std::unexpected(std::move(res).error());
  return {};
}

} // namespace pg
