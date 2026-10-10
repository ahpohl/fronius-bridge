#include "ha_discovery.h"
#include "config.h"
#include <algorithm>
#include <cctype>
#include <format>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

namespace {

// How Home Assistant interprets one reading. The device class selects icon
// and unit conversion; the state class decides whether long-term statistics
// are kept, and total_increasing is what the Energy dashboard accepts for a
// lifetime counter. A null member is left out of the payload.
struct Quantity {
  const char *deviceClass;
  const char *unit;
  const char *stateClass;
};

constexpr Quantity energy{"energy", "kWh", "total_increasing"};
constexpr Quantity reactiveEnergy{"reactive_energy", "kvarh",
                                  "total_increasing"};
// Home Assistant has no device class for apparent energy; unit and state
// class alone still give it statistics.
constexpr Quantity apparentEnergy{nullptr, "kVAh", "total_increasing"};
constexpr Quantity power{"power", "W", "measurement"};
constexpr Quantity apparentPower{"apparent_power", "VA", "measurement"};
constexpr Quantity reactivePower{"reactive_power", "var", "measurement"};
constexpr Quantity powerFactor{"power_factor", "%", "measurement"};
constexpr Quantity powerFactorRatio{"power_factor", nullptr, "measurement"};
constexpr Quantity voltage{"voltage", "V", "measurement"};
constexpr Quantity current{"current", "A", "measurement"};
constexpr Quantity frequency{"frequency", "Hz", "measurement"};
constexpr Quantity percent{nullptr, "%", "measurement"};
constexpr Quantity text{nullptr, nullptr, nullptr};

// Same vocabulary as MqttClient and the masters publish on the availability
// topics; the consumer compares against these literally.
constexpr const char *payloadAvailable = "connected";
constexpr const char *payloadNotAvailable = "disconnected";

constexpr const char *supportUrl = "https://github.com/ahpohl/fronius-bridge";

// Discovery topic levels and every unique_id are restricted to
// [A-Za-z0-9_-]. Device names already are (parseName enforces it), but the
// base topic may contain '/', so it is folded into a single level. It prefixes
// every object id and unique_id, which keeps two bridge instances on one
// broker apart.
std::string baseNodeId(const std::string &baseTopic) {
  std::string id = baseTopic;
  std::ranges::replace_if(
      id,
      [](unsigned char c) { return !std::isalnum(c) && c != '_' && c != '-'; },
      '_');
  return id;
}

std::string nodeId(const std::string &baseTopic, const std::string &name) {
  return baseNodeId(baseTopic) + "_" + name;
}

json origin() {
  return {
      {"name", PROJECT_NAME},
      {"sw_version", PROJECT_VERSION},
      {"support_url", supportUrl},
  };
}

// The device and origin blocks plus the shared options. Availability and
// state_topic are inherited by every component that does not set its own, so
// a component only names a topic when it reads something other than
// `values`. The abbreviated keys (dev, o, cmps, p) are the spelling both the
// Home Assistant and the openHAB documentation use for device discovery.
template <typename Device>
json deviceRoot(const std::string &baseTopic, const std::string &topicBase,
                const std::string &node, const std::string &name,
                const Device &device) {
  json dev;
  dev["identifiers"] = json::array({node});
  dev["name"] = name;
  // An unread or unsupported identity field stays out rather than showing up
  // as an empty string on the device page.
  const auto setIf = [&dev](const char *key, const std::string &value) {
    if (!value.empty())
      dev[key] = value;
  };
  setIf("manufacturer", device.manufacturer);
  setIf("model", device.model);
  setIf("serial_number", device.serialNumber);
  setIf("sw_version", device.fwVersion);
  // Groups the device under the bridge in Home Assistant ("Connected via").
  dev["via_device"] = baseNodeId(baseTopic);

  json root;
  root["dev"] = std::move(dev);
  root["o"] = origin();

  // A reading is live only while both the bridge and the device are: the
  // bridge topic carries the broker-side will, the device topic the Modbus
  // link.
  root["availability"] = json::array();
  for (const auto &topic :
       {baseTopic + "/availability", topicBase + "/availability"}) {
    root["availability"].push_back({
        {"topic", topic},
        {"payload_available", payloadAvailable},
        {"payload_not_available", payloadNotAvailable},
    });
  }
  root["availability_mode"] = "all";
  root["state_topic"] = topicBase + "/values";
  root["cmps"] = json::object();
  return root;
}

// Collects the sensor components of one device under `cmps`. The component
// id doubles as the unique_id suffix, so it must stay stable across releases:
// renaming one orphans the entity and its history in Home Assistant.
class Components {
public:
  Components(json &cmps, std::string node)
      : cmps_(cmps), node_(std::move(node)) {}

  // `field` is the path into the state payload, e.g. "phases[0].ac_voltage".
  // ordered_json keeps its members in a vector, so the returned reference is
  // only good until the next add().
  json &add(const std::string &id, const std::string &name,
            const std::string &field, const Quantity &q) {
    json c;
    c["p"] = "sensor";
    c["name"] = name;
    c["unique_id"] = node_ + "_" + id;
    c["value_template"] = std::format("{{{{ value_json.{} }}}}", field);
    if (q.deviceClass)
      c["device_class"] = q.deviceClass;
    if (q.unit)
      c["unit_of_measurement"] = q.unit;
    if (q.stateClass)
      c["state_class"] = q.stateClass;
    return cmps_[id] = std::move(c);
  }

private:
  json &cmps_;
  std::string node_;
};

// <prefix>/device/<base>_<name>/config, deliberately without the optional
// node id level (<prefix>/device/<base>/<name>/config). With a node id, Home
// Assistant's discovery code names each component "<node> <component>", with
// a space, and openHAB's binding, which runs that same code, turns the name
// into a channel UID and rejects the Thing.
std::string discoveryTopic(const MqttDiscoveryConfig &discovery,
                           const std::string &baseTopic,
                           const std::string &name) {
  return discovery.prefix + "/device/" + nodeId(baseTopic, name) + "/config";
}

} // namespace

namespace HaDiscovery {

Message bridge(const MqttDiscoveryConfig &discovery,
               const std::string &baseTopic) {
  const std::string node = baseNodeId(baseTopic);

  json root;
  root["dev"] = {
      {"identifiers", json::array({node})},
      {"name", baseTopic},
      {"model", PROJECT_NAME},
      {"sw_version", PROJECT_VERSION},
  };
  root["o"] = origin();

  // The bridge availability topic, which the broker-side will turns to
  // `disconnected` on an unclean death, is both the device's availability and
  // the entity's state. openHAB derives a Thing's status only from declared
  // availability; without it the bridge Thing never leaves UNKNOWN. The price
  // is that Home Assistant shows the entity "unavailable" rather than "off"
  // once the bridge is gone.
  root["availability"] = json::array({{
      {"topic", baseTopic + "/availability"},
      {"payload_available", payloadAvailable},
      {"payload_not_available", payloadNotAvailable},
  }});
  root["cmps"] = {
      {"connectivity",
       {
           {"p", "binary_sensor"},
           {"name", "Connectivity"},
           {"unique_id", node + "_connectivity"},
           {"state_topic", baseTopic + "/availability"},
           {"payload_on", payloadAvailable},
           {"payload_off", payloadNotAvailable},
           {"device_class", "connectivity"},
           {"entity_category", "diagnostic"},
       }},
  };

  // <prefix>/device/<base>/config: every device's object id extends <base>
  // with "_<name>", so no device name can collide with the bridge's.
  return {discovery.prefix + "/device/" + node + "/config", root.dump()};
}

Message inverter(const MqttDiscoveryConfig &discovery,
                 const std::string &baseTopic, const InverterConfig &cfg,
                 const InverterTypes::Device &device) {
  const std::string topicBase = baseTopic + "/inverter/" + cfg.name;
  const std::string node = nodeId(baseTopic, cfg.name);

  json root = deviceRoot(baseTopic, topicBase, node, cfg.name, device);
  Components c(root["cmps"], node);

  c.add("ac_energy", "AC energy", "ac_energy", energy);
  c.add("ac_power_active", "AC power", "ac_power_active", power);
  c.add("ac_power_apparent", "AC apparent power", "ac_power_apparent",
        apparentPower);
  c.add("ac_power_reactive", "AC reactive power", "ac_power_reactive",
        reactivePower);
  c.add("ac_power_factor", "AC power factor", "ac_power_factor", powerFactor);
  c.add("ac_frequency", "AC frequency", "ac_frequency", frequency);
  c.add("dc_power", "DC power", "dc_power", power);
  c.add("efficiency", "Efficiency", "efficiency", percent);

  // Clamped exactly like the values payload, so every template indexes an
  // array element that is actually published.
  const int phases = std::clamp(device.phases, 1, 3);
  for (int i = 0; i < phases; ++i) {
    const int n = i + 1;
    c.add(std::format("phase{}_ac_voltage", n),
          std::format("Phase {} voltage", n),
          std::format("phases[{}].ac_voltage", i), voltage);
    c.add(std::format("phase{}_ac_current", n),
          std::format("Phase {} current", n),
          std::format("phases[{}].ac_current", i), current);
  }

  // The values payload carries no per-input energy for a hybrid inverter;
  // announcing it anyway would leave the entity permanently unknown.
  const int inputs = std::clamp(device.inputs, 1, 2);
  for (int i = 0; i < inputs; ++i) {
    const int n = i + 1;
    c.add(std::format("input{}_dc_voltage", n),
          std::format("Input {} voltage", n),
          std::format("inputs[{}].dc_voltage", i), voltage);
    c.add(std::format("input{}_dc_current", n),
          std::format("Input {} current", n),
          std::format("inputs[{}].dc_current", i), current);
    c.add(std::format("input{}_dc_power", n), std::format("Input {} power", n),
          std::format("inputs[{}].dc_power", i), power);
    if (!device.isHybrid)
      c.add(std::format("input{}_dc_energy", n),
            std::format("Input {} energy", n),
            std::format("inputs[{}].dc_energy", i), energy);
  }

  // The operating state is the one events field worth a dashboard; the
  // vendor code and event list are for diagnosing a fault.
  const std::string eventsTopic = topicBase + "/events";
  c.add("state", "State", "state", text)["state_topic"] = eventsTopic;

  json &code = c.add("active_code", "Status code", "active_code", text);
  code["state_topic"] = eventsTopic;
  code["entity_category"] = "diagnostic";

  json &events = c.add("events", "Events", "events | join(', ')", text);
  events["state_topic"] = eventsTopic;
  events["entity_category"] = "diagnostic";

  return {discoveryTopic(discovery, baseTopic, cfg.name), root.dump()};
}

Message meter(const MqttDiscoveryConfig &discovery,
              const std::string &baseTopic, const MeterConfig &cfg,
              const MeterTypes::Device &device) {
  const std::string topicBase = baseTopic + "/meter/" + cfg.name;
  const std::string node = nodeId(baseTopic, cfg.name);

  json root = deviceRoot(baseTopic, topicBase, node, cfg.name, device);
  Components c(root["cmps"], node);

  c.add("energy_active_import", "Energy import", "energy_active_import",
        energy);
  c.add("energy_active_export", "Energy export", "energy_active_export",
        energy);
  c.add("energy_reactive_import", "Reactive energy import",
        "energy_reactive_import", reactiveEnergy);
  c.add("energy_reactive_export", "Reactive energy export",
        "energy_reactive_export", reactiveEnergy);
  c.add("energy_apparent_import", "Apparent energy import",
        "energy_apparent_import", apparentEnergy);
  c.add("energy_apparent_export", "Apparent energy export",
        "energy_apparent_export", apparentEnergy);

  // The EBZ publishes its configured grid.power_factor as a ratio, while a
  // Fronius meter reports percent; announce what each payload carries.
  const bool fronius = asFronius(cfg) != nullptr;
  const Quantity &pf = fronius ? powerFactor : powerFactorRatio;

  c.add("power_active", "Power", "power_active", power);
  c.add("power_apparent", "Apparent power", "power_apparent", apparentPower);
  c.add("power_reactive", "Reactive power", "power_reactive", reactivePower);
  c.add("power_factor", "Power factor", "power_factor", pf);
  c.add("frequency", "Frequency", "frequency", frequency);
  c.add("voltage_ph", "Voltage", "voltage_ph", voltage);

  // Phase-to-phase voltage does not exist on a single-phase meter, where the
  // payload carries a constant 0.
  const int phases = std::clamp(device.phases, 1, 3);
  const bool polyphase = phases > 1;
  if (polyphase)
    c.add("voltage_pp", "Line voltage", "voltage_pp", voltage);

  // The EBZ telegram has no total current, so its values payload omits it.
  if (fronius)
    c.add("current", "Current", "current", current);

  for (int i = 0; i < phases; ++i) {
    const int n = i + 1;
    const auto add = [&](const char *field, const char *label,
                         const Quantity &q) {
      c.add(std::format("phase{}_{}", n, field),
            std::format("Phase {} {}", n, label),
            std::format("phases[{}].{}", i, field), q);
    };
    add("power_active", "power", power);
    add("power_apparent", "apparent power", apparentPower);
    add("power_reactive", "reactive power", reactivePower);
    add("power_factor", "power factor", pf);
    add("voltage_ph", "voltage", voltage);
    if (polyphase)
      add("voltage_pp", "line voltage", voltage);
    add("current", "current", current);
  }

  return {discoveryTopic(discovery, baseTopic, cfg.name), root.dump()};
}

} // namespace HaDiscovery
