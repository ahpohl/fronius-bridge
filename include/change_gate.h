#ifndef CHANGE_GATE_H_
#define CHANGE_GATE_H_

#include <optional>

// Change detector used by the device producers (the inverter/meter masters)
// to emit device metadata only when it actually changes instead of on every
// poll or telegram. T must be equality-comparable (the Device structs define
// a defaulted operator==).
//
// Not thread-safe by design: each master owns one gate and touches it only
// from its own poll thread (runLoop).
template <typename T> class ChangeGate {
public:
  // Returns true and records `value` as the new baseline when it differs from
  // the last accepted value, or when nothing has been accepted yet. Returns
  // false when `value` equals the last accepted value.
  bool changed(const T &value) {
    if (last_ && *last_ == value)
      return false;
    last_ = value;
    return true;
  }

  // Whether any value has been accepted yet. Producers for which reading the
  // device identity is expensive (Modbus over a shared bus) use this to read
  // once and skip the re-read on later polls; producers that re-observe it for
  // free every cycle (SML telegrams) rely on changed() alone.
  bool hasValue() const noexcept { return last_.has_value(); }

  // Discard the accepted value so the next changed() reports a change and
  // hasValue() reads false again. Scopes the read-once short-circuit to a
  // single connection instead of the whole process lifetime.
  void reset() noexcept { last_.reset(); }

private:
  std::optional<T> last_;
};

#endif /* CHANGE_GATE_H_ */
