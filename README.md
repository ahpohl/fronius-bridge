[![Build](https://github.com/ahpohl/fronius-bridge/actions/workflows/build.yml/badge.svg)](https://github.com/ahpohl/fronius-bridge/actions/workflows/build.yml)

# fronius-bridge

fronius-bridge is a lightweight service that reads operational data from Fronius inverters and publishes it to MQTT as JSON. It supports both Modbus TCP (IPv4/IPv6) and Modbus RTU (serial) connections[...]

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

- Topic: fronius-bridge/values
  ```jsonc
  {
    "time": 1762607887640,
    "ac_energy": 11060.2,
    "ac_power_active": 238.0,
    "ac_power_apparent": 238.1,
    "ac_power_reactive": 5.0,
    "ac_power_factor": -100.0,
    "phases": [
      {
        "id": 1,
        "ac_voltage": 235.9,
        "ac_current": 1.0
      }
    ],
    "ac_frequency": 50.0,
    "dc_power": 285.2,
    "efficiency": 83.4,
    "inputs": [
      {
        "id": 1,
        "dc_voltage": 294.2,
        "dc_current": 0.45,
        "dc_power": 132.4,
        "dc_energy": 5468.4
      },
      {
        "id": 2,
        "dc_voltage": 293.9,
        "dc_current": 0.52,
        "dc_power": 152.8,
        "dc_energy": 0.1
      }
    ]
  }
  ```

- Topic: fronius-bridge/events
  ```json
  {
    "active_code": 0,
    "state": "Tracking power point",
    "events": []
  }
  ```

- Topic: fronius-bridge/device
  ```jsonc
  {
    "data_manager": "3.32.1-2",
    "firmware_version": "0.3.30.2",
    "hybrid": false,
    "inverter_id": 101,
    "manufacturer": "Fronius",
    "model": "Primo 4.0-1",
    "mppt_tracker": 2,
    "phases": 1,
    "power_rating": 4000.0, // VA (apparent power rating)
    "register_model": "int+sf", // or "float"
    "serial_number": "34119102",
    "slave_id": 1
  }
  ```

### Field reference

| Field                         | Description                                               | Units | Notes |
|------------------------------|-----------------------------------------------------------|-------|-------|
| time                         | Timestamp (Unix epoch)                                    | ms    | UTC milliseconds since epoch |
| ac_energy                    | Cumulative AC energy                                      | Wh    | Sourced from inverter counter |
| ac_power_active              | Active AC power                                           | W     | Instantaneous |
| ac_power_apparent            | Apparent AC power                                         | VA    | Instantaneous |
| ac_power_reactive            | Reactive AC power                                         | var   | Instantaneous |
| ac_power_factor              | Power factor                                              | %     | Typically range -100..100; sign per inverter convention |
| phases[].id                  | Phase index                                               | —     | Starts at 1 |
| phases[].ac_voltage          | Per-phase AC voltage                                      | V     | — |
| phases[].ac_current          | Per-phase AC current                                      | A     | — |
| ac_frequency                 | AC frequency                                              | Hz    | — |
| dc_power                     | Total DC input power                                      | W     | Sum of inputs |
| efficiency                   | Conversion efficiency                                     | %     | Computed as ac_power_active / dc_power * 100; 0 when dc_power ≈ 0 |
| inputs[].id                  | DC input index                                            | —     | Starts at 1 |
| inputs[].dc_voltage          | DC input voltage                                          | V     | — |
| inputs[].dc_current          | DC input current                                          | A     | — |
| inputs[].dc_power            | DC input power                                            | W     | — |
| inputs[].dc_energy           | Cumulative DC energy per input                            | Wh    | Omitted on hybrid models; sourced from inverter counter |
| active_code                  | Inverter active state code                                | —     | Events JSON |
| state                        | Inverter state string                                     | —     | Events JSON |
| events                       | Array of event strings                                    | —     | Events JSON; may be empty |
| manufacturer                 | Inverter manufacturer                                     | —     | Device JSON |
| model                        | Inverter model                                            | —     | Device JSON |
| serial_number                | Inverter serial number                                    | —     | Device JSON |
| firmware_version             | Inverter firmware version                                 | —     | Device JSON |
| data_manager                 | Data manager/options version                              | —     | Device JSON |
| register_model               | Register model used                                       | —     | "float" or "int+sf" |
| hybrid                       | Hybrid/storage capable                                    | —     | Battery currently not supported |
| mppt_tracker                 | Number of DC inputs / MPPT trackers                       | —     | Device JSON |
| phases                       | Number of AC phases                                       | —     | Device JSON |
| power_rating                 | Apparent power rating                                     | VA    | Device JSON |
| inverter_id                  | Inverter numeric ID                                       | —     | Device JSON |
| slave_id                     | Inverter Modbus address                                   | —     | Device JSON |

### Power factor sign convention

ac_power_factor is provided as a percentage exactly as reported by the inverter via the Fronius register map. Typical interpretation is:
- Positive values for lagging (inductive) load
- Negative values for leading (capacitive) feed-in

The observed numeric range is approximately -100..100. If your installation uses a different sign convention, refer to the official Fronius documentation for your model and firmware.

### Energy counters

- ac_energy and inputs[].dc_energy are cumulative counters maintained by the inverter. The application does not reset these values on restart.
- inputs[].dc_energy is omitted on hybrid models.
- Units are Wh after internal scaling.
- If you need per-session or per-interval deltas, compute them in your consumer by differencing successive readings.

### MQTT publish defaults

- QoS: 1
- Retained: true
- Duplicate suppression: the publisher suppresses consecutive duplicates per topic (hash comparison of payload).
- Queueing: messages are queued per topic up to mqtt.queue_size and published when connected; reconnect uses exponential backoff as configured.
- Consumers should be prepared to receive retained messages on subscribe and handle at-least-once delivery semantics.

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