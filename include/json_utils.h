#ifndef JSON_UTILS_H_
#define JSON_UTILS_H_

#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

// --- Custom wrapper for per-value precision
struct PreciseDouble {
  double value;
  int precision;
};

// --- Custom serializer (called automatically by nlohmann::json)
inline void to_json(json &j, const PreciseDouble &pd) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(pd.precision) << pd.value;
  j = oss.str();
}

#endif /* JSON_UTILS_H_ */
