[![Build](https://github.com/ahpohl/fronius-bridge/actions/workflows/build.yml/badge.svg)](https://github.com/ahpohl/fronius-bridge/actions/workflows/build.yml)

# fronius-bridge

fronius-bridge is a lightweight service that reads operational data from Fronius inverters and publishes it to MQTT as JSON. It supports both Modbus TCP (IPv4/IPv6) and Modbus RTU (serial) connections, automatically detects device characteristics, and is fully configurable via a YAML file.

## Features

- Reads inverter values such as power and energy
- Supports Modbus over TCP (IPv4/IPv6) and serial RTU
- Manages night-time disconnections when the inverter enters standby and resumes publishing automatically.
- Publishes values, events, and device info as JSON messages to an MQTT broker
- Fully configurable through a YAML configuration file
- Extensive, module-scoped logging
- Automatic detection of:
  - Register model
  - Number of phases
  - MPPT tracker inputs
  - Hybrid/storage capability (battery currently not supported)
- PostgreSQL consumer (planned)

## Status and limitations

- Battery/storage data is detected but not yet supported.
- A PostgreSQL consumer is planned but not implemented yet.
- TLS in libmosquitto not yet supported.

## Dependencies

- [libfronius](https://github.com/ahpohl/libfronius) — Reads values, events, and device info from the inverter
- [libmosquitto](https://mosquitto.org/) — MQTT client library
- [yaml-cpp](https://github.com/jbeder/yaml-cpp) — YAML configuration parsing
- [spdlog](https://github.com/gabime/spdlog) — Structured logging

Ensure the development headers for the above libraries are installed on your system.

## Configuration

fronius-bridge is configured via a YAML file. Below is a complete example followed by a field-by-field reference.

### Example config

```yaml
modbus:
  tcp:
    host: primo.home.arpa
    port: 502
  rtu:
    device: /dev/ttyUSB0
    baud: 9600
  slave_id: 1
  response_timeout:
    sec: 5
    usec: 0
  update_interval: 4
  reconnect_delay:
    min: 5
    max: 320
    exponential: true
      
mqtt:
  broker: localhost
  port: 1883
  topic: fronius-bridge
  #user: mqtt
  #password: "your-secret-password"
  queue_size: 100
  reconnect_delay:
    min: 2
    max: 64
    exponential: true
  
logger:
  level: info     # global default: off | error | warn | info | debug | trace
  modules:
    main: info
    modbus: info
    mqtt: debug

```

### Configuration reference

- modbus
  - Note: Configure at least one transport (tcp or rtu). If both are configured, TCP takes precedence over RTU.
  - tcp
    - host: Hostname or IP (IPv4/IPv6) of the inverter or Modbus TCP gateway.
    - port: TCP port for Modbus (default is usually 502).
  - rtu
    - device: Serial device path (e.g., /dev/ttyUSB0).
    - baud: Baud rate for RTU (e.g., 9600, 19200, 38400).
  - slave_id: Modbus unit/slave ID of the inverter (typically 1).
  - response_timeout
    - sec: Seconds part of the Modbus response timeout.
    - usec: Microseconds part of the Modbus response timeout.
    Notes:
    - Total timeout = sec + usec.
    - Increase if you see frequent timeouts on slow/latent links.
  - update_interval: Polling interval in seconds between reads from the inverter.
  - reconnect_delay
    - min: Initial delay (seconds) before attempting to reconnect after a connection error.
    - max: Maximum delay (seconds) between reconnect attempts.
    - exponential: If true, uses exponential backoff between min and max; if false, uses a fixed delay.
    
- mqtt
  - broker: Hostname or IP of the MQTT broker.
  - port: MQTT broker port (1883 for unencrypted, 8883 for TLS, if supported by your setup).
  - topic: Base MQTT topic to publish under (e.g., fronius-bridge). Subtopics may be used for values/events/device info.
  - user: Optional username for broker authentication.
  - password: Optional password for broker authentication.
  - queue_size: Size of the internal publish queue. Increase if bursts of data may outpace network/broker temporarily.
  - reconnect_delay
    - min: Initial delay (seconds) before reconnecting to MQTT after a failure.
    - max: Maximum delay (seconds) between reconnect attempts.
    - exponential: If true, uses exponential backoff between min and max; if false, uses a fixed delay.

- logger
  - level: Global default log level. Accepted values: off, error, warn, info, debug, trace.
  - modules: Per-module overrides for log levels.
    - main: Log level for the main module.
    - modbus: Log level for Modbus transport/communication.
    - mqtt: Log level for MQTT client interactions.
  Notes:
  - A module’s level overrides the global level for that module.
  - Use debug/trace when troubleshooting connectivity or protocol issues.

## MQTT publishing

- Messages are published as JSON strings under the configured base topic.
- Subtopics include values (telemetry), events (faults/alarms), and device info (static metadata).
- Consumers should handle retained/non-retained semantics as configured by your deployment (and broker defaults).

### Topics and example payloads

**fronius-bridge/values**
```jsonc
{
  "time": 1699459200000,           // ms since Unix epoch (UTC)
  "ac_power": 4523.5,               // W
  "ac_current": 19.67,              // A
  "ac_voltage": 230.1,              // V
  "ac_frequency": 50.01,            // Hz
  "dc_current": 10.23,              // A
  "dc_voltage": 442.8,              // V
  "dc_power": 4531.2,               // W
  "dc_energy": 12345678,            // Wh (omitted on hybrid models)
  "ac_energy": 12340000,            // Wh
  "efficiency": 99.83,              // %
  "reactive_power": -123.4,         // var
  "power_factor": 99.9              // % (range -100..100)
}
```

**fronius-bridge/events**
```json
{
  "time": 1699459200000,
  "state_code": 7,
  "state_message": "Running",
  "error_code": 0,
  "error_message": ""
}
```

**fronius-bridge/device**
```jsonc
{
  "time": 1699459200000,           // ms since Unix epoch (UTC)
  "device_type": 123,
  "power_rating": 5000,             // VA
  "register_model": 1,
  "phases": 1,
  "mppt_trackers": 2,
  "has_hybrid": false
}
```

> **Note**: Inline comments shown above (e.g., `// W`) are for documentation purposes only and are **not** included in actual MQTT payloads.

### Field reference

| Field | Description | Units | Notes |
|-------|-------------|-------|-------|
| **values topic** |
| time | Timestamp when measurement was taken | ms since Unix epoch (UTC) | Common to all topics |
| ac_power | AC output power | W | Instantaneous measurement |
| ac_current | AC output current | A | Per-phase or aggregate depending on model |
| ac_voltage | AC output voltage | V | Per-phase or aggregate depending on model |
| ac_frequency | Grid frequency | Hz | Typically 50 or 60 Hz |
| dc_current | DC input current | A | Sum of all MPPT trackers |
| dc_voltage | DC input voltage | V | Typically from dominant MPPT |
| dc_power | DC input power | W | Sum of all MPPT trackers |
| dc_energy | Cumulative DC energy produced | Wh | Omitted on hybrid inverter models |
| ac_energy | Cumulative AC energy produced | Wh | Total energy delivered to grid/loads |
| efficiency | Conversion efficiency (DC to AC) | % | Instantaneous efficiency |
| reactive_power | Reactive power | var | Positive or negative |
| power_factor | Power factor | % | Range: -100 to +100 |
| **events topic** |
| time | Timestamp of event | ms since Unix epoch (UTC) | |
| state_code | Numeric inverter state code | - | See inverter documentation |
| state_message | Human-readable state | - | e.g., "Running", "Standby" |
| error_code | Numeric error/fault code | - | 0 = no error |
| error_message | Human-readable error description | - | Empty if no error |
| **device topic** |
| time | Timestamp of device info snapshot | ms since Unix epoch (UTC) | |
| device_type | Fronius device type code | - | Model-specific identifier |
| power_rating | Nominal inverter power rating | VA | Maximum apparent power |
| register_model | Modbus register layout version | - | Used for register mapping |
| phases | Number of AC phases | - | 1 or 3 |
| mppt_trackers | Number of MPPT inputs | - | Typically 1 or 2 |
| has_hybrid | Indicates hybrid/battery capability | - | Battery support planned |

### Power factor sign convention

The `power_factor` field follows the sign convention used by Fronius inverters:

- **Positive values** (+1 to +100%): Inductive load (current lags voltage).
- **Negative values** (-1 to -100%): Capacitive load (current leads voltage).
- **Range**: -100 to +100, where ±100 represents unity power factor (purely resistive).

The sign indicates the phase relationship between voltage and current, not the quality of the power factor.

### Energy counters

- **ac_energy** and **dc_energy** are cumulative counters representing the total energy produced since the inverter was commissioned.
- Values are in **Wh** (watt-hours) and increment over the lifetime of the inverter.
- **dc_energy is omitted** on hybrid inverter models due to register layout differences.
- To calculate energy production over a time interval, consumers must difference successive readings (e.g., `energy_interval = current_reading - previous_reading`).

### MQTT publish defaults

fronius-bridge publishes messages with the following characteristics:

- **QoS 1** (at least once delivery): Ensures messages are reliably delivered even if the broker temporarily disconnects.
- **Retained messages**: By default, messages are published with the retained flag set, so new subscribers immediately receive the last known values/events/device info.
- **Duplicate suppression**: Messages are hashed; if the payload is identical to the previous message for a given topic, it is not republished (reduces broker/network load).
- **Per-topic queueing**: Each topic (values, events, device) has an independent queue to prevent head-of-line blocking.
- **Reconnect backoff**: If the broker connection is lost, fronius-bridge uses exponential backoff (configurable via `mqtt.reconnect_delay`) to avoid overwhelming the broker during recovery.

## Troubleshooting

- Connection timeouts:
  - Increase modbus.response_timeout or modbus.update_interval.
  - Verify slave_id and transport (TCP vs RTU) match your inverter setup.
- Frequent reconnects:
  - Check broker reachability and credentials.
  - Adjust mqtt.reconnect_delay and modbus.reconnect_delay backoff ranges.
- Missing data fields:
  - Some fields depend on inverter model/features (e.g., hybrid/battery).

## Security considerations

- Prefer running MQTT behind a trusted network or VPN.
- If using authentication, set mqtt.user and mqtt.password and store the config file with appropriate permissions.

## License

[MIT](LICENSE)

---

*fronius-bridge* is not affiliated with or endorsed by Fronius International GmbH.