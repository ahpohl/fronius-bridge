#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "config_yaml.h"
#include "signal_handler.h"
#include <atomic>
#include <condition_variable>
#include <mosquitto.h>
#include <mutex>
#include <queue>
#include <spdlog/logger.h>
#include <string>
#include <thread>
#include <unordered_map>

class MqttClient {
public:
  MqttClient(const MqttConfig &cfg, SignalHandler &signalHandler);
  ~MqttClient();

  // Producer pushes JSON payloads here
  void publish(std::string payload, const std::string &topic);

private:
  void run();

  // Drives mosquitto_loop() with our own reconnect/backoff, replacing
  // mosquitto_loop_start() (whose thread quits on a TLS/protocol rejection).
  void networkLoop();

  // Final bounded write pass, run by networkLoop() after its main loop.
  void drainOutgoing();

  MqttConfig cfg_;

  // Bridge-level availability, `<mqtt.topic>/availability`. MQTT allows one
  // will per connection, so the per-device topics cannot each have one;
  // consumers AND this with the per-device topic to decide whether a device's
  // reading is live.
  std::string availabilityTopic_;

  // Logger
  std::shared_ptr<spdlog::logger> logger_;

  // State
  std::atomic<bool> connected_{false};

  // Both loops run for as long as this object lives, not until the signal, so
  // the availability publishes issued by the masters' destructors are still
  // drained. Cleared by ~MqttClient(), which runs after every master.
  std::atomic<bool> active_{true};

  // Set by run() once its final flush pass has returned; tells drainOutgoing()
  // there is nothing more coming.
  std::atomic<bool> flushDone_{false};

  struct mosquitto *mosq_ = nullptr;
  std::thread worker_;
  std::thread networkThread_;
  SignalHandler &handler_;
  std::mutex mutex_;
  std::condition_variable cv_;

  // --- queued messages setup
  struct QueuedMessage {
    std::string payload;
  };
  std::map<std::string, std::queue<QueuedMessage>> topicQueues_;
  std::unordered_map<std::string, std::size_t> lastPayloadHashes_;
  std::map<std::string, size_t> droppedCount_;
  bool hasQueuedMessages() const;

  // --- callbacks
  static void onConnect(struct mosquitto *mosq, void *obj, int rc);
  static void onDisconnect(struct mosquitto *mosq, void *obj, int rc);
  static void onLog(struct mosquitto *mosq, void *obj, int level,
                    const char *str);
};

#endif // MQTT_CLIENT_H
