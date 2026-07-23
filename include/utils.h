#ifndef UTILS_H_
#define UTILS_H_

#include <cmath>

namespace Utils {

// Round a double to a fixed number of decimal places. Used by the value
// structs' round() to bound output precision before values reach consumers.
inline double roundTo(double value, int decimals) {
  double factor = std::pow(10.0, decimals);
  return std::round(value * factor) / factor;
}

// Scale a base SI unit to its kilo prefix: Wh -> kWh, VAh -> kVAh,
// varh -> kvarh. Applied at every downstream sink (MQTT publish, PostgreSQL
// insert). The in-memory Values structs keep Wh-family units because that
// matches the SunSpec register semantics libfronius uses and what its Modbus
// slave servers emit, so every master fills its struct in Wh regardless of how
// its source reports energy and every consumer scales at the boundary.
//
// Divide by 1000.0 (exactly representable) rather than multiplying by 1e-3
// (which is not): for the whole-Wh values Values::round() produces, the
// division yields the exact 3-dp kWh, avoiding artefacts such as
// 167301 Wh -> 167.30100000000002 kWh.
constexpr double scaleToKilo(double base) { return base / 1000.0; }

} // namespace Utils

#endif /* UTILS_H_ */
