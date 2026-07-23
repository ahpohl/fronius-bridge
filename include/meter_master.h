#ifndef METER_MASTER_H_
#define METER_MASTER_H_

#include "change_gate.h"
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
//   - availability callback: a connectivity state string, emitted on
//                            transition only (see publishAvailability)
//
// Concrete subclasses (FroniusMeter, EasyMeter) implement the transport and
// their own worker thread. main.cpp holds masters through this base so the
// callback wiring is identical regardless of meter kind.
//
// Callback storage and cbMutex_ live here so the locking discipline is shared
// rather than re-implemented per subclass; the setters are non-virtual, so
// subclasses decide only when the callbacks fire, not how they are stored.
// Availability goes through publishAvailability() so the emit-on-transition
// contract has one implementation for every meter kind.
//
// Lifetime: a master owns a thread that may invoke these callbacks, so it must
// outlive any object they touch. Subclass destructors join their worker and
// remove any bus callbacks before base teardown; the virtual destructor makes
// deletion through a base pointer correct.
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

  // Publish an availability state ("connected"/"disconnected") through the
  // gate, so each distinct state is emitted once on transition. Decide under
  // the lock (the gate is reached from the transport thread and from the
  // subclass destructor), then fire outside it. Unwired: the short-circuit
  // skips changed(), so nothing latches and a later publish still fires.
  void publishAvailability(std::string state) {
    bool emit;
    {
      std::lock_guard<std::mutex> lock(cbMutex_);
      emit = availabilityCallback_ && availabilityGate_.changed(state);
    }
    if (emit)
      availabilityCallback_(std::move(state));
  }

  // Guards the three callbacks below (and is reused by subclasses to guard
  // their own data that is published alongside a callback invocation).
  // mutable so const accessors in subclasses may lock it.
  mutable std::mutex cbMutex_;

  std::function<void(std::string, MeterTypes::Values)> valueCallback_;
  std::function<void(std::string, MeterTypes::Device)> deviceCallback_;
  std::function<void(std::string)> availabilityCallback_;

  // Guarded by cbMutex_, unlike the subclasses' single-threaded device gates:
  // a meter reports availability from whichever thread noticed the change, and
  // once more from its destructor.
  ChangeGate<std::string> availabilityGate_;
};

#endif /* METER_MASTER_H_ */
