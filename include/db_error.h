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
// TRANSIENT/FATAL severity gradient that determines whether a failure
// triggers reconnect (TRANSIENT) or process shutdown (FATAL). The SQLSTATE
// (when available from libpq) is preserved for diagnostics and used to
// classify severity.
//
// Severity is deduced from (Kind, sqlstate) at construction time. Use the
// `make`/`makeWithState` factories for the normal case; use the `*Severity`
// overloads when a specific call site has reason to override the default
// (e.g. internal validation logic that wants to log-and-continue rather
// than escalate). Once a DbError is constructed its severity is fixed.
// ---------------------------------------------------------------------------

struct DbError {
  enum class Kind {
    CONNECT,   // failed to establish a connection
    MIGRATION, // schema migration failed
    QUERY,     // a query/statement failed at runtime
    PROTOCOL,  // protocol-level (broken_connection etc.)
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

  // Return the first line of `s` (everything before the first '\n', or all
  // of `s` if there is no newline). Used to keep multi-line libpq server
  // messages out of one-line log messages: the first line is the primary
  // error description; the trailing lines are server context that is more
  // noise than signal at the log level.
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
  // Maps (kind, sqlstate) to a severity. The defaults are deliberately
  // conservative against false-FATAL: anything we cannot positively
  // identify as a configuration/programming error is classified as
  // TRANSIENT so that a misclassified error leads to a retry rather than
  // a shutdown.
  //
  //   CONNECT, PROTOCOL  -> TRANSIENT (server unreachable / link dropped)
  //   QUERY              -> TRANSIENT by default; FATAL for SQLSTATE
  //                         classes that indicate the schema, privileges,
  //                         or database state are misconfigured (operator
  //                         must fix, retry won't help)
  //   MIGRATION          -> FATAL by default; TRANSIENT only when the
  //                         SQLSTATE indicates a lost connection - any
  //                         schema / permission / feature error during a
  //                         migration is deterministic and needs the
  //                         operator's attention
  //   INTERNAL           -> FATAL (programming bug)
  //
  // SQLSTATE class reference (PostgreSQL):
  //   08  Connection Exception              - TRANSIENT
  //   0A  Feature Not Supported             - FATAL (e.g. missing extension)
  //   28  Invalid Authorization             - FATAL
  //   3D  Invalid Catalog Name (no DB)      - FATAL
  //   42  Syntax / Access Rule Violation    - FATAL (incl. 42501 priv)
  //   53  Insufficient Resources            - TRANSIENT (disk full may clear)
  //   57  Operator Intervention             - TRANSIENT (admin shutdown etc.)
  // Everything else (40 deadlock, 55 object-in-use, ...) is left as the
  // kind's default; for QUERY that is TRANSIENT, which is the right
  // behaviour for retryable transactional failures.
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
    case Kind::INTERNAL:
      return "INTERNAL";
    }
    return "UNKNOWN";
  }
};

#endif /* DB_ERROR_H_ */
