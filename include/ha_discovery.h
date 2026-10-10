#ifndef HA_DISCOVERY_H
#define HA_DISCOVERY_H

#include "config_yaml.h"
#include "inverter_types.h"
#include "meter_types.h"
#include <string>

// ---------------------------------------------------------------------------
// Home Assistant MQTT device discovery
//
// Builds one retained config message per device, published to
// `<prefix>/device/<base>_<name>/config`, that describes every reading of the
// device's existing `values` (and, for inverters, `events`) payload as a
// sensor entity. Nothing about the state payloads changes: each entity reads
// its field through a value_template, so Grafana, Node-RED and the database
// consumer see exactly what they did before.
//
// The descriptor is an input because phase and MPPT counts, the hybrid flag
// and the manufacturer/model/serial are only known after the first device
// read; the caller publishes from the device callback.
// ---------------------------------------------------------------------------

namespace HaDiscovery {

struct Message {
  std::string topic;
  std::string payload;
};

// The bridge itself, as the parent every inverter and meter names in
// `via_device`, with a connectivity entity on the bridge availability topic.
// Depends on nothing read from hardware, so it is published once at startup.
Message bridge(const MqttDiscoveryConfig &discovery,
               const std::string &baseTopic);

Message inverter(const MqttDiscoveryConfig &discovery,
                 const std::string &baseTopic, const InverterConfig &cfg,
                 const InverterTypes::Device &device);

Message meter(const MqttDiscoveryConfig &discovery,
              const std::string &baseTopic, const MeterConfig &cfg,
              const MeterTypes::Device &device);

} // namespace HaDiscovery

#endif // HA_DISCOVERY_H
