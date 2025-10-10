#ifndef MATH_UTILS_H_
#define MATH_UTILS_H_

#include <cmath>
#include <spdlog/spdlog.h>
#include <string>

inline double
safeDivide(double numerator, double denominator,
           spdlog::logger *logger = nullptr,
           const std::string &warnMsg =
               "Math error: attempted division by zero or near-zero",
           double defaultValue = 0.0, double epsilon = 1e-12) {
  if (std::abs(denominator) > epsilon) {
    return numerator / denominator;
  } else {
    if (logger) {
      logger->warn("{}", warnMsg);
    }
    return defaultValue;
  }
}

#endif /* MATH_UTILS_H_ */
