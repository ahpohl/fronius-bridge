[![Build](https://github.com/ahpohl/fronius-bridge/actions/workflows/build.yml/badge.svg)](https://github.com/ahpohl/fronius-bridge/actions/workflows/build.yml)

# fronius-bridge

fronius-bridge is a lightweight service that reads operational data from Fronius inverters and smart meters and publishes it to MQTT as JSON. It supports both Modbus TCP (IPv4/IPv6) and Modbus RTU (serial) connections.

## Features

- Reads inverter values such as power and energy
- Reads smart meter values (Fronius TS65-a and SunSpec-compatible meters)
- Supports Modbus over TCP (IPv4/IPv6) and serial RTU
- Manages night-time disconnections when the inverter enters standby and resumes publishing automatically
- Publishes values, events, device info and connection availability as JSON messages to an MQTT broker
- Fully configurable through a YAML configuration file
- Extensive, module-scoped logging
- Automatic detection of:
  - Register model (inverter and meter)
  - Number of phases
  - MPPT tracker inputs
  - Hybrid/storage capability (battery currently not supported)

## Status and limitations

- Battery/storage data is detected but not yet supported.
- A PostgreSQL consumer is planned but not implemented yet.
- TLS in libmosquitto not yet supported.
- Shared RTU bus (inverter master and meter master on the same physical serial port) is planned but not yet supported.

## Dependencies

- [libfronius](https://github.com/ahpohl/libfronius) — Reads values, events, and device info from the inverter and meter
- [libmosquitto](https://mosquitto.org/) — MQTT client library
- [yaml-cpp](https://github.com/jbeder/yaml-cpp) — YAML configuration parsing
- [spdlog](https://github.com/gabime/spdlog) — Structured logging

Ensure the development headers for the above libraries are installed on your system.

## Configuration

fronius-bridge is configured via a YAML file. Below is a complete example followed by a field-by-field reference.

> **Note:** The `inverter` and `meter` sections are both optional, but **at least one must be configured**. The service will refuse to start if neither is present. Any combination is valid: inverter only, meter only, or both together.

### Example config

```yaml
inverter:
  tcp:
    host: primo.home.arpa
    port: 502
  rtu:
    device: /dev/ttyUSB0
    baud: 9600
    data_bits: 8
    stop_bits: 1
    parity: none
  unit_id: 1
  response_timeout:
    sec: 5
    usec: 0
  update_interval: 4
  reconnect_delay:
    min: 5
    max: 320
    exponential: true

meter:
  master:
    tcp:
      host: localhost
      port: 502
    rtu:
      device: /dev/ttyUSB1
      baud: 9600
      data_bits: 8
      stop_bits: 1
      parity: none
    unit_id: 1
    response_timeout:
      sec: 5
      usec: 0
    update_interval: 4
    reconnect_delay:
      min: 5
      max: 320
      exponential: true
  slave:
    tcp:
      listen: 0.0.0.0
      port: 502
    rtu:
      device: /dev/ttyUSB2
      baud: 9600
      data_bits: 8
      stop_bits: 1
      parity: none
    unit_id: 1
    request_timeout: 5
    idle_timeout: 60
    use_float_model: false

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
    inverter: info
    mqtt: debug
    meter:
      master: info
      slave: info
```

### Configuration reference

> Both `inverter` and `meter` are optional top-level sections. At least one must be present — the service exits at startup with an error if neither is configured.

- inverter *(optional)*
  - Note: Configure at least one transport (tcp or rtu). If both are configured, TCP takes precedence over RTU.
  - tcp
    - host: Hostname or IP (IPv4/IPv6) of the inverter or Modbus TCP gateway.
    - port: TCP port for Modbus (default: 502).
  - rtu
    - device: Serial device path (e.g. /dev/ttyUSB0).
    - baud: Baud rate (e.g. 9600, 19200, 38400).
    - data_bits: Data bits — 5, 6, 7, or 8.
    - stop_bits: Stop bits — 1 or 2.
    - parity: Parity — `none`, `even`, or `odd`.
  - unit_id: Modbus unit/slave ID of the inverter (typically 1).
  - response_timeout
    - sec: Seconds part of the Modbus response timeout.
    - usec: Microseconds part of the Modbus response timeout.
    - Notes: Total timeout = sec + usec. Increase if you see frequent timeouts on slow/latent links.
  - update_interval: Polling interval in seconds between reads from the inverter.
  - reconnect_delay
    - min: Initial delay (seconds) before attempting to reconnect after a connection error.
    - max: Maximum delay (seconds) between reconnect attempts.
    - exponential: If true, uses exponential backoff between min and max; if false, uses a fixed delay.

- meter *(optional)*
  - Note: When present, `meter.master` is mandatory — it is the source of meter register data. `meter.slave` is optional and can be configured independently of the master.
  - master — fronius-bridge acts as a Modbus master (client) and reads meter register data. Two register models are supported and are detected automatically on connect:
    - Fronius TS65-a proprietary register map — used when connecting directly to a Fronius smart meter over RTU.
    - SunSpec — standard register model, supported over both TCP and RTU. Covers meters accessed via the inverter's Modbus TCP proxy interface as well as standalone SunSpec-compliant meters such as Easymeter devices exposed through a smartmeter-gateway.
  - Configure at least one transport (tcp or rtu) under `meter.master`. TCP takes precedence if both are present.
    - tcp
      - host: Hostname or IP of the Modbus slave (inverter proxy or smartmeter-gateway).
      - port: Modbus TCP port (default: 502).
    - rtu
      - device: Serial device path (e.g. /dev/ttyUSB1).
      - baud: Baud rate.
      - data_bits, stop_bits, parity: Serial framing — see inverter.rtu for accepted values.
    - unit_id: Modbus unit/slave ID of the remote meter. Use 1 for a directly connected meter; use 240 for the primary meter proxied through the inverter (241 for secondary, per Fronius Datamanager specification).
    - response_timeout: See inverter.response_timeout for guidance.
    - update_interval: Polling interval in seconds between meter reads.
    - reconnect_delay: Same semantics as inverter.reconnect_delay.
  - slave — fronius-bridge exposes a SunSpec-compliant Modbus slave (server) so that a Fronius inverter or another Modbus master can read meter values back from it. This is useful when the inverter requires a smart meter on the bus but the physical meter is not directly accessible from it. `meter.master` must also be configured — without it the slave has no values to serve. Configure at least one transport (tcp or rtu). TCP takes precedence if both are present.
    - tcp
      - listen: Bind address (IPv4 or IPv6). Use `0.0.0.0` for all IPv4 interfaces or `::` for all IPv6 interfaces.
      - port: TCP port to listen on (default: 502). Note: binding to ports below 1024 requires elevated privileges or `CAP_NET_BIND_SERVICE`.
    - rtu
      - device: Serial device path.
      - baud, data_bits, stop_bits, parity: Serial framing — see inverter.rtu for accepted values.
    - unit_id: Modbus unit ID this slave responds to (typically 1).
    - request_timeout: Maximum time (seconds) to wait between requests from the master before considering the session stalled.
    - idle_timeout: Disconnect a TCP client that sends no activity for this many seconds. Has no effect on RTU transport.
    - use_float_model: Register model to expose. `false` (default) exposes integer + scale-factor registers compatible with the Fronius int+sf register map. `true` exposes 32-bit IEEE 754 float registers.

- mqtt
  - broker: Hostname or IP of the MQTT broker.
  - port: MQTT broker port (1883 for unencrypted, 8883 for TLS, if supported by your setup).
  - topic: Base MQTT topic to publish under (e.g. `fronius-bridge`). Subtopics are appended automatically — see [MQTT publishing](#mqtt-publishing).
  - user: Optional username for broker authentication.
  - password: Optional password for broker authentication.
  - queue_size: Size of the internal publish queue. Increase if bursts of data may outpace the network or broker temporarily.
  - reconnect_delay
    - min: Initial delay (seconds) before reconnecting to MQTT after a failure.
    - max: Maximum delay (seconds) between reconnect attempts.
    - exponential: If true, uses exponential backoff between min and max; if false, uses a fixed delay.

- logger
  - level: Global default log level. Accepted values: `off`, `error`, `warn`, `info`, `debug`, `trace`.
  - modules: Per-module overrides. A module's level takes precedence over the global level.
    - main: Application startup, signal handling, lifecycle.
    - inverter: Inverter Modbus reads and register parsing.
    - mqtt: MQTT client connect/disconnect/publish.
    - meter.master: Meter master (client) Modbus reads.
    - meter.slave: Meter slave (server) Modbus register serving.
  - Notes: Use `debug`/`trace` when troubleshooting connectivity or protocol issues. `trace` produces verbose frame/register dumps and should be used sparingly.

## Supported topologies

The `inverter` and `meter` sections are independent and can be combined freely — either or both may be configured, but at least one must be present.

### Common topology

The diagram below shows the recommended setup when both inverter and smart meter are present. `inverter.master` reads the Fronius inverter over Modbus TCP. `meter.master` reads the TS65-a directly over Modbus RTU via a USB RS-485 adapter using the proprietary register map. `meter.slave` re-serves the meter values back to the inverter as a SunSpec-compliant Modbus TCP server, so the inverter can use the meter for feed-in management. Both masters publish to an external MQTT broker via the integrated MQTT client.

![Common topology](docs/topology-common.svg)

### Other supported configurations

**Inverter only (no meter)**  
Omit the `meter` section entirely. The inverter can be reached over Modbus TCP (IPv4/IPv6) or Modbus RTU via a USB RS-485 adapter. If both transports are configured, TCP takes precedence.

**Meter only (no inverter)**  
Omit the `inverter` section. Useful for standalone meter monitoring without an inverter on the bus.

**Meter behind inverter (TCP proxy)**  
Instead of a direct RTU connection, configure `meter.master` with TCP pointing at the inverter's IP and unit ID 240 (the primary energy meter's fixed Modbus device ID per the Fronius Datamanager specification; secondary meters start at 241). The inverter forwards requests to the meter on its internal RS-485 port. No USB dongle is required, and the SunSpec register model is used automatically. `meter.slave` is not needed in this case since the inverter already has direct access to the meter.

**Meter slave without inverter master**  
`meter.slave` can run without `inverter` being configured at all. fronius-bridge reads the meter via `meter.master` and serves the values over TCP, while a separate inverter or other Modbus master queries the slave independently.

**Not yet supported: shared RTU bus**  
A topology where both `inverter.master` and `meter.master` connect to the same physical RS-485 serial dongle is planned but not yet implemented. Currently each must use a separate serial device.

## Smart meter support

fronius-bridge supports the Fronius TS65-a smart meter and any SunSpec-compatible meter through two complementary roles: **meter master** and **meter slave**.

### Register model auto-discovery

On each connection the meter master probes the remote device and automatically selects the correct register model — no manual configuration is required:

- **Fronius TS65-a proprietary** — selected when connecting directly to a Fronius smart meter over Modbus RTU. The meter exposes its native proprietary register map on the RS-485 bus.
- **SunSpec** — selected for all other cases: meters accessed via the inverter's built-in Modbus TCP proxy, standalone SunSpec-compliant meters (e.g. Easymeter via smartmeter-gateway), or any other device that identifies itself with the SunSpec well-known identifier.

## MQTT publishing

Messages are published as JSON strings under the configured base topic. When both inverter and meter are configured, each component publishes to its own subtopic namespace.

| Component | Subtopic                        | Content                         |
|-----------|---------------------------------|---------------------------------|
| Inverter  | `<topic>/inverter/values`       | Telemetry (power, energy, etc.) |
| Inverter  | `<topic>/inverter/events`       | Faults and alarms               |
| Inverter  | `<topic>/inverter/device`       | Static device metadata          |
| Inverter  | `<topic>/inverter/availability` | Connection state                |
| Meter     | `<topic>/meter/values`          | Telemetry (power, energy, etc.) |
| Meter     | `<topic>/meter/device`          | Static device metadata          |
| Meter     | `<topic>/meter/availability`    | Connection state                |

### Topics and example payloads

- Topic: `<topic>/inverter/values`
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

- Topic: `<topic>/inverter/events`
  ```json
  {
    "active_code": 0,
    "state": "Tracking power point",
    "events": []
  }
  ```

- Topic: `<topic>/inverter/device`
  ```jsonc
  {
    "data_manager": "3.32.1-2",
    "firmware_version": "0.3.30. 2",
    "hybrid": false,
    "inverter_id": 101,
    "manufacturer": "Fronius",
    "model": "Primo 4.0-1",
    "mppt_tracker": 2,
    "phases": 1,
    "power_rating": 4000.0,
    "register_model": "int+sf",
    "serial_number": "34119102",
    "slave_id": 1
  }
  ```

- Topic: `<topic>/inverter/availability` / `<topic>/meter/availability`
  ```
  connected
  ```
  or
  ```
  disconnected
  ```

- Topic: `<topic>/meter/values`
  ```jsonc
  {
    "time": 1762607887640,
    "energy_active_import": 3521.847,
    "energy_active_export": 8042.113,
    "energy_apparent_import": 3980.201,
    "energy_apparent_export": 8510.774,
    "energy_reactive_import": 120.034,
    "energy_reactive_export": 410.882,
    "power_active": -1842.0,
    "power_apparent": 1843.0,
    "power_reactive": 45.0,
    "power_factor": -99.9,
    "frequency": 50.0,
    "voltage_ph": 234.2,
    "voltage_pp": 0.0,
    "current": 7.864,
    "phases": [
      {
        "id": 1,
        "power_active": -1842.0,
        "power_apparent": 1843.0,
        "power_reactive": 45.0,
        "power_factor": -99.9,
        "voltage_ph": 234.2,
        "voltage_pp": 0.0,
        "current": 7.864
      }
    ]
  }
  ```

- Topic: `<topic>/meter/device`
  ```jsonc
  {
    "manufacturer": "Fronius",
    "model": "TS65-a-3",
    "serial_number": "12345678",
    "firmware_version": "1.3.0",
    "data_manager": "",
    "register_model": "proprietary",
    "slave_id": 1,
    "meter_id": 203,
    "phases": 1
  }
  ```

### Inverter field reference

| Field                        | Description                                               | Units | Notes |
|------------------------------|-----------------------------------------------------------|-------|-------|
| time                         | Timestamp (Unix epoch)                                    | ms    | UTC milliseconds since epoch |
| ac_energy                    | Cumulative AC energy                                      | Wh    | Sourced from inverter counter |
| ac_power_active              | Active AC power                                           | W     | — |
| ac_power_apparent            | Apparent AC power                                         | VA    | — |
| ac_power_reactive            | Reactive AC power                                         | var   | — |
| ac_power_factor              | Power factor                                              | %     | Typically range -100..100; sign per inverter convention |
| phases[].id                  | Phase index                                               | —     | Starts at 1 |
| phases[].ac_voltage          | Per-phase AC voltage                                      | V     | — |
| phases[].ac_current          | Per-phase AC current                                      | A     | — |
| ac_frequency                 | AC frequency                                              | Hz    | — |
| dc_power                     | Total DC input power                                      | W     | Sum of inputs |
| efficiency                   | Conversion efficiency                                     | %     | Computed as ac_power_active / dc_power * 100; 0 when dc_power ≈ 0 |
| inputs[].id                  | DC input index                                            | —     | Starts at 1 |
| inputs[].dc_voltage          | DC input voltage                                          | V     | — |
| inputs[].dc_current          | DC input current                                         | A     | — |
| inputs[].dc_power            | DC input power                                            | W     | — |
| inputs[].dc_energy           | Cumulative DC energy per input                            | Wh    | Omitted on hybrid models |
| active_code                  | Inverter active state code                                | —     | — |
| state                        | Inverter state string                                     | —     | — |
| events                       | Array of event strings                                    | —     | May be empty |
| manufacturer                 | Inverter manufacturer                                     | —     | — |
| model                        | Inverter model                                            | —     | — |
| serial_number                | Inverter serial number                                    | —     | — |
| firmware_version             | Inverter firmware version                                 | —     | — |
| data_manager                 | Data manager/options version                              | —     | — |
| register_model               | Register model used                                       | —     | `float` or `int+sf` |
| hybrid                       | Hybrid/storage capable                                    | —     | Battery currently not supported |
| mppt_tracker                 | Number of DC inputs / MPPT trackers                       | —     | — |
| phases                       | Number of AC phases                                       | —     | — |
| power_rating                 | Apparent power rating                                     | VA    | — |
| inverter_id                  | Inverter numeric ID                                       | —     | — |
| slave_id                     | Inverter Modbus address                                   | —     | — |

### Meter field reference

| Field                        | Description                                               | Units | Notes |
|------------------------------|-----------------------------------------------------------|-------|-------|
| time                         | Timestamp (Unix epoch)                                    | ms    | UTC milliseconds since epoch |
| energy_active_import         | Cumulative active energy imported from grid               | kWh   | — |
| energy_active_export         | Cumulative active energy exported to grid                 | kWh   | — |
| energy_apparent_import       | Cumulative apparent energy imported                       | kVAh  | — |
| energy_apparent_export       | Cumulative apparent energy exported                       | kVAh  | — |
| energy_reactive_import       | Cumulative reactive energy imported                       | kvarh | — |
| energy_reactive_export       | Cumulative reactive energy exported                       | kvarh | — |
| power_active                 | Total active power                                        | W     | Negative = export |
| power_apparent               | Total apparent power                                      | VA    | — |
| power_reactive               | Total reactive power                                      | var   | — |
| power_factor                 | Total power factor                                        | %     | Range -100..100 |
| frequency                    | AC frequency                                              | Hz    | — |
| voltage_ph                   | Phase-to-neutral voltage (total/average)                  | V     | — |
| voltage_pp                   | Phase-to-phase voltage (total/average)                    | V     | 0.0 on single-phase installations |
| current                      | Total current                                             | A     | — |
| phases[].id                  | Phase index                                               | —     | Starts at 1 |
| phases[].power_active        | Per-phase active power                                    | W     | Negative = export |
| phases[].power_apparent      | Per-phase apparent power                                  | VA    | — |
| phases[].power_reactive      | Per-phase reactive power                                  | var   | — |
| phases[].power_factor        | Per-phase power factor                                    | %     | — |
| phases[].voltage_ph          | Per-phase phase-to-neutral voltage                        | V     | — |
| phases[].voltage_pp          | Per-phase phase-to-phase voltage                          | V     | — |
| phases[].current             | Per-phase current                                         | A     | — |
| manufacturer                 | Meter manufacturer                                        | —     | — |
| model                        | Meter model                                               | —     | — |
| serial_number                | Meter serial number                                       | —     | — |
| firmware_version             | Meter firmware version                                    | —     | — |
| data_manager                 | Options/data manager version                              | —     | Empty string if not applicable |
| register_model               | Register model detected on connect                        | —     | `proprietary` (TS65-a) or `sunspec` |
| meter_id                     | Meter numeric ID                                          | —     | — |
| slave_id                     | Meter Modbus address                                      | —     | — |
| phases                       | Number of AC phases                                       | —     | Determines length of phases array |
| availability                 | Meter connection state                                    | —     | `connected` or `disconnected` |

### Power sign convention

`power_active` follows the load convention: positive values indicate energy consumed from the grid (import), negative values indicate energy fed into the grid (export). The same sign applies to per-phase `power_active` fields.

`power_factor` is reported as a percentage in the range -100..100, consistent with the inverter's convention.

### Energy counters

- All energy fields (`energy_active_import`, `energy_active_export`, etc.) are cumulative counters maintained by the physical meter and are published in kWh/kVAh/kvarh. The application does not reset these values on restart.
- If you need per-session or per-interval deltas, compute them in your consumer by differencing successive readings.

### MQTT publish defaults

- QoS: 1
- Retained: true
- Duplicate suppression: the publisher suppresses consecutive duplicates per topic (hash comparison of payload).
- Queueing: messages are queued per topic up to `mqtt.queue_size` and published when connected; reconnect uses exponential backoff as configured.
- Consumers should be prepared to receive retained messages on subscribe and handle at-least-once delivery semantics.

## Troubleshooting

- Connection timeouts:
  - Increase `inverter.response_timeout` or `inverter.update_interval`.
  - Verify `unit_id` and transport (TCP vs RTU) match your device setup.
  - For meter master, verify the serial framing parameters (`baud`, `data_bits`, `stop_bits`, `parity`) match the physical meter.
- Frequent reconnects:
  - Check broker reachability and credentials.
  - Adjust `mqtt.reconnect_delay` and the appropriate `reconnect_delay` section.
- Missing meter data:
  - Confirm `meter.master` is configured — `meter.slave` alone does not read any data.
  - Check the `meter.master` log at `debug` level to see which register model was detected on connect.
- Meter slave not responding to inverter:
  - Verify `meter.slave.unit_id` matches the unit ID the inverter is querying.
  - If using TCP, confirm the bind address and port are reachable from the inverter.
  - Check `meter.slave.use_float_model` — Fronius inverters typically require `false` (int+sf).

## Security considerations

- Prefer running MQTT behind a trusted network or VPN.
- If using authentication, set `mqtt.user` and `mqtt.password` and store the config file with appropriate permissions (e.g. `chmod 0600`).
- Binding `meter.slave.tcp.port` to a port below 1024 requires elevated privileges or `CAP_NET_BIND_SERVICE`.

## License

[MIT](LICENSE)

---

*fronius-bridge* is not affiliated with or endorsed by Fronius International GmbH.
