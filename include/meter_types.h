#ifndef METER_TYPES_H_
#define METER_TYPES_H_

#include <cstdint>
#include <string>

#include "utils.h"

struct MeterTypes {

  // --- Meter value types ---
  struct Phase {
    double phVoltage{0.0};
    double ppVoltage{0.0};
    double current{0.0};
    double activePower{0.0};
    double reactivePower{0.0};
    double apparentPower{0.0};
    double powerFactor{0.0};

    // Quantise to the output precision (voltage 1 dp, current 3 dp, power and
    // power factor 2 dp) so every consumer sees the same value.
    void round() {
      phVoltage = Utils::roundTo(phVoltage, 1);
      ppVoltage = Utils::roundTo(ppVoltage, 1);
      current = Utils::roundTo(current, 3);
      activePower = Utils::roundTo(activePower, 2);
      reactivePower = Utils::roundTo(reactivePower, 2);
      apparentPower = Utils::roundTo(apparentPower, 2);
      powerFactor = Utils::roundTo(powerFactor, 2);
    }
  };

  struct Values {
    uint64_t time{0};
    double activeEnergyImport{0.0};
    double activeEnergyExport{0.0};
    double reactiveEnergyImport{0.0};
    double reactiveEnergyExport{0.0};
    double apparentEnergyImport{0.0};
    double apparentEnergyExport{0.0};
    double phVoltage{0.0};
    double ppVoltage{0.0};
    double current{0.0};
    double activePower{0.0};
    double reactivePower{0.0};
    double apparentPower{0.0};
    double powerFactor{0.0};
    double frequency{0.0};
    Phase phase1;
    Phase phase2;
    Phase phase3;

    // Quantise to the output precision before the values reach any consumer.
    // Energies are in Wh and rounded to whole Wh (0 dp), which becomes 3 dp
    // once scaled to kWh; voltage 1 dp, current 3 dp, power / power factor /
    // frequency 2 dp.
    void round() {
      activeEnergyImport = Utils::roundTo(activeEnergyImport, 0);
      activeEnergyExport = Utils::roundTo(activeEnergyExport, 0);
      reactiveEnergyImport = Utils::roundTo(reactiveEnergyImport, 0);
      reactiveEnergyExport = Utils::roundTo(reactiveEnergyExport, 0);
      apparentEnergyImport = Utils::roundTo(apparentEnergyImport, 0);
      apparentEnergyExport = Utils::roundTo(apparentEnergyExport, 0);
      phVoltage = Utils::roundTo(phVoltage, 1);
      ppVoltage = Utils::roundTo(ppVoltage, 1);
      current = Utils::roundTo(current, 3);
      activePower = Utils::roundTo(activePower, 2);
      reactivePower = Utils::roundTo(reactivePower, 2);
      apparentPower = Utils::roundTo(apparentPower, 2);
      powerFactor = Utils::roundTo(powerFactor, 2);
      frequency = Utils::roundTo(frequency, 2);
      phase1.round();
      phase2.round();
      phase3.round();
    }
  };

  struct Device {
    int id{0};
    std::string manufacturer;
    std::string model;
    std::string serialNumber;
    std::string fwVersion;
    std::string options;
    std::string dataManagerVersion;
    std::string registerModel;
    int phases{0};
    int inputs{0};
    int slaveID{0};

    bool operator==(const Device &) const = default;
  };

  enum class ErrorAction { NONE, RECONNECT, SHUTDOWN };
};

#endif /* METER_TYPES_H_ */