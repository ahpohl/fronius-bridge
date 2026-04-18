#ifndef METER_TYPES_H_
#define METER_TYPES_H_

#include <cstdint>
#include <string>

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
  };

  enum class ErrorAction { NONE, RECONNECT, SHUTDOWN };
};

#endif /* METER_TYPES_H_ */