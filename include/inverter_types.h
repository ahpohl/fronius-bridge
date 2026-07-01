#ifndef INVERTER_TYPES_H_
#define INVERTER_TYPES_H_

#include "utils.h"
#include <cstdint>
#include <string>
#include <vector>

struct InverterTypes {

  struct Input {
    double dcVoltage{0.0};
    double dcCurrent{0.0};
    double dcPower{0.0};
    double dcEnergy{0.0};

    // Voltage 1 dp, current 3 dp, power 2 dp; dcEnergy is Wh rounded to whole
    // Wh (0 dp), which becomes 3 dp once scaled to kWh.
    void round() {
      dcVoltage = Utils::roundTo(dcVoltage, 1);
      dcCurrent = Utils::roundTo(dcCurrent, 3);
      dcPower = Utils::roundTo(dcPower, 2);
      dcEnergy = Utils::roundTo(dcEnergy, 0);
    }
  };

  struct Phase {
    double acVoltage{0.0};
    double acCurrent{0.0};

    void round() {
      acVoltage = Utils::roundTo(acVoltage, 1);
      acCurrent = Utils::roundTo(acCurrent, 3);
    }
  };

  struct Values {
    uint64_t time{0};
    double acEnergy{0.0};
    double acPowerActive{0.0};
    double acPowerApparent{0.0};
    double acPowerReactive{0.0};
    double acPowerFactor{0.0};
    Phase phase1;
    Phase phase2;
    Phase phase3;
    double acFrequency{0.0};
    double dcPower{0.0};
    double efficiency{0.0};
    Input input1;
    Input input2;

    // Quantise to the output precision before the values reach any consumer.
    // acEnergy is Wh rounded to whole Wh (0 dp -> 3 dp in kWh); power /
    // power factor / frequency / efficiency 2 dp.
    void round() {
      acEnergy = Utils::roundTo(acEnergy, 0);
      acPowerActive = Utils::roundTo(acPowerActive, 2);
      acPowerApparent = Utils::roundTo(acPowerApparent, 2);
      acPowerReactive = Utils::roundTo(acPowerReactive, 2);
      acPowerFactor = Utils::roundTo(acPowerFactor, 2);
      acFrequency = Utils::roundTo(acFrequency, 2);
      dcPower = Utils::roundTo(dcPower, 2);
      efficiency = Utils::roundTo(efficiency, 2);
      phase1.round();
      phase2.round();
      phase3.round();
      input1.round();
      input2.round();
    }
  };

  struct Events {
    int activeCode{0};               // Fronius F_Active_State_Code
    std::string state;               // Inverter StVnd
    std::vector<std::string> events; // Inverter EvtVnd1-3

    // Member-wise equality so ChangeGate can de-duplicate the whole snapshot.
    bool operator==(const Events &) const = default;
  };

  struct Device {
    int id{0};
    std::string manufacturer;
    std::string model;
    std::string serialNumber;
    std::string fwVersion;
    std::string dataManagerVersion;
    std::string registerModel;
    bool isHybrid{false};
    int phases{0};
    int inputs{0};
    int slaveID{0};
    double acPowerApparent{0.0};

    bool operator==(const Device &) const = default;
  };
};

#endif /* INVERTER_TYPES_H_ */