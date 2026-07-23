#ifndef DB_ERROR_H_
#define DB_ERROR_H_

#include <exception>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// ---------------------------------------------------------------------------
// DbError
//
// Error type for the PostgreSQL consumer. Mirrors ModbusError, including the
// TRANSIENT/FATAL severity gradient that decides whether a failure triggers a
// reconnect or a process shutdown. The SQLSTATE, when libpq supplies one, is
// preserved for diagnostics and used to classify severity.
//
// Severity is deduced from (Kind, sqlstate) at construction. Use the
// `make`/`makeWithState` factories normally; the `*Severity` overloads let a
// call site override the default (e.g. validation logic that wants to
// log-and-continue). Once constructed, a DbError's severity is fixed.
// ---------------------------------------------------------------------------

struct DbError {
  enum class Kind {
    CONNECT,   // failed to establish a connection
    MIGRATION, // schema migration failed
    QUERY,     // a query/statement failed at runtime
    PROTOCOL,  // protocol-level (broken_connection etc.)
    IDENTITY,  // reported device identity is not the one the schema holds
    INTERNAL,  // logic / programming error
  };

  enum class Severity {
    TRANSIENT, // expected to clear on retry: reconnect, drop event
    FATAL,     // requires operator intervention: shut the process down
  };

  Kind kind{Kind::INTERNAL};
  Severity severity{Severity::FATAL};
  std::string message;
  std::optional<std::string> sqlstate;

  std::string describe() const {
    return sqlstate ? std::format("[{}] {} (SQLSTATE {})", toString(kind),
                                  message, *sqlstate)
                    : std::format("[{}] {}", toString(kind), message);
  }

  // --- Factories with deduced severity ---

  template <typename... Args>
  static DbError make(Kind kind, std::format_string<Args...> fmt,
                      Args &&...args) {
    return DbError{kind, deduceSeverity(kind, std::nullopt),
                   std::format(fmt, std::forward<Args>(args)...), std::nullopt};
  }

  template <typename... Args>
  static DbError makeWithState(Kind kind, std::string_view sqlstate,
                               std::format_string<Args...> fmt,
                               Args &&...args) {
    auto severity = deduceSeverity(kind, sqlstate);
    return DbError{kind, severity,
                   std::format(fmt, std::forward<Args>(args)...),
                   std::string{sqlstate}};
  }

  // Return the first line of `s`. Keeps multi-line libpq server messages out
  // of one-line log entries: the first line is the primary error description,
  // the rest is server context.
  static constexpr std::string_view firstLine(std::string_view s) noexcept {
    if (auto nl = s.find('\n'); nl != std::string_view::npos)
      return s.substr(0, nl);
    return s;
  }

  // Build a DbError from a std::exception, taking only the first line of
  // what() as the message (see firstLine above for rationale).
  static DbError fromException(const std::exception &e, Kind kind) {
    return DbError::make(kind, "{}", firstLine(e.what()));
  }

  // --- Factories with explicit severity (overrides deduction) ---

  template <typename... Args>
  static DbError makeSeverity(Kind kind, Severity severity,
                              std::format_string<Args...> fmt, Args &&...args) {
    return DbError{kind, severity,
                   std::format(fmt, std::forward<Args>(args)...), std::nullopt};
  }

  template <typename... Args>
  static DbError
  makeWithStateSeverity(Kind kind, Severity severity, std::string_view sqlstate,
                        std::format_string<Args...> fmt, Args &&...args) {
    return DbError{kind, severity,
                   std::format(fmt, std::forward<Args>(args)...),
                   std::string{sqlstate}};
  }

  // --- Severity classification ---
  //
  // Maps (kind, sqlstate) to a severity, biased against false-FATAL: anything
  // not positively identified as a configuration or programming error stays
  // TRANSIENT, so a misclassification retries rather than shuts down.
  //
  //   CONNECT, PROTOCOL  -> TRANSIENT (server unreachable / link dropped)
  //   QUERY              -> TRANSIENT, except the FATAL classes below
  //   MIGRATION          -> FATAL, except a lost connection (class 08)
  //   INTERNAL           -> FATAL (programming bug)
  //
  // SQLSTATE classes that decide those exceptions:
  //   08  Connection Exception              - TRANSIENT
  //   0A  Feature Not Supported             - FATAL (e.g. missing extension)
  //   28  Invalid Authorization             - FATAL
  //   3D  Invalid Catalog Name (no DB)      - FATAL
  //   42  Syntax / Access Rule Violation    - FATAL (incl. 42501 priv)
  // Everything else keeps the kind's default, which for QUERY is TRANSIENT -
  // the right behaviour for retryable transactional failures.
  static constexpr Severity
  deduceSeverity(Kind kind, std::optional<std::string_view> sqlstate) noexcept {
    // Connection-class SQLSTATE always wins: it means the link dropped,
    // regardless of which kind the call site labeled it. Reconnect handles
    // it cleanly even mid-migration.
    if (sqlstate && sqlstate->size() >= 2 &&
        std::string_view{*sqlstate}.substr(0, 2) == "08") {
      return Severity::TRANSIENT;
    }

    switch (kind) {
    case Kind::CONNECT:
    case Kind::PROTOCOL:
      return Severity::TRANSIENT;

    case Kind::INTERNAL:
      return Severity::FATAL;

    case Kind::IDENTITY:
      // Not recoverable by retrying, but scoped to one device: the consumer
      // quarantines that schema and keeps writing the others rather than
      // letting one misconfigured device stop every other device's data.
      return Severity::TRANSIENT;

    case Kind::MIGRATION:
      // Anything except a connection drop during migration is fatal -
      // see SQLSTATE 08 short-circuit above.
      return Severity::FATAL;

    case Kind::QUERY:
      if (sqlstate && sqlstate->size() >= 2) {
        const std::string_view cls{std::string_view{*sqlstate}.substr(0, 2)};
        if (cls == "0A" || cls == "28" || cls == "3D" || cls == "42")
          return Severity::FATAL;
      }
      return Severity::TRANSIENT;
    }
    return Severity::FATAL;
  }

private:
  static constexpr std::string_view toString(Kind k) {
    switch (k) {
    case Kind::CONNECT:
      return "CONNECT";
    case Kind::MIGRATION:
      return "MIGRATION";
    case Kind::QUERY:
      return "QUERY";
    case Kind::PROTOCOL:
      return "PROTOCOL";
    case Kind::IDENTITY:
      return "IDENTITY";
    case Kind::INTERNAL:
      return "INTERNAL";
    }
    return "UNKNOWN";
  }
};

#endif /* DB_ERROR_H_ */
