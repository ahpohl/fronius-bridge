#ifndef CHANGE_GATE_H_
#define CHANGE_GATE_H_

#include <optional>

// Change detector shared by the device producers (the inverter/meter masters)
// to emit device metadata only when it actually changes instead of on every
// poll or telegram. Holds the last accepted value of T and reports whether a
// new value differs from it. T must be equality-comparable (the Device structs
// define a defaulted operator==).
//
// Not thread-safe by design: each master owns one gate and touches it only
// from its own poll thread (runLoop), exactly where the previous per-master
// "device updated" latch was used.
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

  // Whether any value has been accepted yet. A producer whose transport makes
  // reading device identity expensive (Modbus over a shared bus) uses this to
  // read once and short-circuit the re-read on later polls; a producer that
  // re-observes identity for free every cycle (SML telegrams) ignores it and
  // relies on changed() alone.
  bool hasValue() const noexcept { return last_.has_value(); }

private:
  std::optional<T> last_;
};

#endif /* CHANGE_GATE_H_ */
