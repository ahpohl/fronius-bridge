#ifndef INVERTER_TYPES_H_
#define INVERTER_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

struct InverterTypes {

  struct Input {
    double dcVoltage{0.0};
    double dcCurrent{0.0};
    double dcPower{0.0};
    double dcEnergy{0.0};
  };

  struct Phase {
    double acVoltage{0.0};
    double acCurrent{0.0};
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
  };

  struct Events {
    int activeCode{0};               // Fronius F_Active_State_Code
    std::string state;               // Inverter StVnd
    std::vector<std::string> events; // Inverter EvtVnd1-3
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
  };
};

#endif /* INVERTER_TYPES_H_ */