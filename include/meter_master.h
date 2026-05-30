#ifndef METER_MASTER_H_
#define METER_MASTER_H_

#include "meter_types.h"
#include <functional>
#include <mutex>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// MeterMaster — abstract base for the wire-side reader of a single meter.
//
// A "meter master" owns whatever transport reads a physical meter (Modbus for
// Fronius SunSpec meters, a serial SML/OBIS stream for the EBZ Easymeter) and
// surfaces three streams to the application via callbacks:
//
//   - value callback:        live measurements (MeterTypes::Values)
//   - device callback:       device identity / nameplate (MeterTypes::Device)
//   - availability callback: a connectivity state string
//
// Concrete subclasses (FroniusMeter, EasyMeter) implement the
// transport and their own worker thread; they invoke the stored callbacks
// when fresh data arrives. main.cpp holds masters through this base so the
// callback-wiring is identical regardless of meter kind.
//
// Callback storage and the mutex that guards it live here in the base so the
// locking discipline is shared and not re-implemented per subclass. The three
// setters are non-virtual: subclasses must not change how callbacks are
// stored, only when they are fired. Subclasses read the callbacks under
// cbMutex_ from their worker thread.
//
// Lifetime: a master owns a thread that may invoke these callbacks, so a
// master must outlive any object its callbacks touch. Subclass destructors
// are responsible for joining their worker (and removing any bus callbacks)
// before base teardown; the virtual destructor guarantees correct
// destruction through a base pointer.
// ---------------------------------------------------------------------------

class MeterMaster {
public:
  virtual ~MeterMaster() = default;

  // Non-copyable, non-movable — subclasses own a thread.
  MeterMaster(const MeterMaster &) = delete;
  MeterMaster &operator=(const MeterMaster &) = delete;
  MeterMaster(MeterMaster &&) = delete;
  MeterMaster &operator=(MeterMaster &&) = delete;

  // Install the callbacks invoked by the subclass worker thread. Thread-safe;
  // each replaces any previously-installed callback under cbMutex_.
  void
  setValueCallback(std::function<void(std::string, MeterTypes::Values)> cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    valueCallback_ = std::move(cb);
  }
  void
  setDeviceCallback(std::function<void(std::string, MeterTypes::Device)> cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    deviceCallback_ = std::move(cb);
  }
  void setAvailabilityCallback(std::function<void(std::string)> cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    availabilityCallback_ = std::move(cb);
  }

protected:
  MeterMaster() = default;

  // Guards the three callbacks below (and is reused by subclasses to guard
  // their own data that is published alongside a callback invocation).
  // mutable so const accessors in subclasses may lock it.
  mutable std::mutex cbMutex_;

  std::function<void(std::string, MeterTypes::Values)> valueCallback_;
  std::function<void(std::string, MeterTypes::Device)> deviceCallback_;
  std::function<void(std::string)> availabilityCallback_;
};

#endif /* METER_MASTER_H_ */
