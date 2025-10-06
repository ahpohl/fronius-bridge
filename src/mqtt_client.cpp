#include "mqtt_client.h"
#include "config_yaml.h"
#include "signal_handler.h"
#include <mosquitto.h>
#include <spdlog/spdlog.h>

MqttClient::MqttClient(const MqttConfig &cfg, SignalHandler &signalHandler)
    : cfg_(cfg), handler_(signalHandler) {

  // Setup mqtt logger
  mqttLogger_ = spdlog::get("mqtt");
  if (!mqttLogger_)
    mqttLogger_ = spdlog::default_logger();

  // Create Mosquitto client
  mosquitto_lib_init();
  mosq_ = mosquitto_new(nullptr, true, this);
  if (!mosq_) {
    mqttLogger_->critical("Failed to create mosquitto client");
    handler_.notify();
  }

  // Set username/password if provided
  if (cfg_.user.has_value()) {
    mosquitto_username_pw_set(mosq_, opt_c_str(cfg_.user),
                              opt_c_str(cfg_.password));
  }

  // Configure automatic reconnect delay
  mosquitto_reconnect_delay_set(mosq_, cfg_.reconnectDelay->min,
                                cfg_.reconnectDelay->max,
                                cfg_.reconnectDelay->exponential);

  // Set Mosquitto callbacks
  mosquitto_connect_callback_set(mosq_, MqttClient::onConnect);
  mosquitto_disconnect_callback_set(mosq_, MqttClient::onDisconnect);
  mosquitto_log_callback_set(mosq_, MqttClient::onLog);

  // Start Mosquitto network loop
  int rc = mosquitto_loop_start(mosq_);
  if (rc != MOSQ_ERR_SUCCESS) {
    mqttLogger_->critical("Failed to start mosquitto network loop: {} ({})",
                          mosquitto_strerror(rc), rc);
    handler_.notify();
  }

  // Connect to broker
  rc = mosquitto_connect_async(mosq_, opt_c_str(cfg_.broker), cfg_.port, 60);
  if (rc != MOSQ_ERR_SUCCESS) {
    mqttLogger_->warn("MQTT: initial connect failed (async): {}",
                      mosquitto_strerror(rc));
  }

  // Start worker thread to process queue
  worker_ = std::thread(&MqttClient::run, this);
}

MqttClient::~MqttClient() {
  cv_.notify_all();
  if (worker_.joinable())
    worker_.join();

  if (mosq_) {
    mosquitto_disconnect(mosq_);
    mosquitto_loop_stop(mosq_, true);
    mosquitto_destroy(mosq_);
    mosq_ = nullptr;
  }

  mosquitto_lib_cleanup();
}

void MqttClient::publish(const std::string &payload) {
  std::unique_lock<std::mutex> lock(mutex_);

  if (queue_.size() >= cfg_.queueSize) {
    queue_.pop();    // remove oldest
    droppedCount_++; // track total drops
  }

  queue_.push(payload); // push new message

  // Logging only if disconnected
  if (!connected_.load()) {
    if (droppedCount_ > 0) {
      mqttLogger_->warn(
          "MQTT queue full, dropped oldest message (total dropped: {})",
          droppedCount_);
    } else {
      if (queue_.size() > 0)
        mqttLogger_->debug(
            "Waiting for MQTT connection... ({} messages cached)",
            queue_.size());
    }
  }

  cv_.notify_one(); // wake up the consumer thread
}

void MqttClient::run() {
  while (handler_.isRunning()) {
    std::unique_lock<std::mutex> lock(mutex_);

    // --- First wait until connected or shutdown ---
    cv_.wait(lock, [&] { return connected_.load() || !handler_.isRunning(); });
    if (!handler_.isRunning())
      break;

    // --- Then wait until we have something to publish or shutdown ---
    cv_.wait(lock, [&] { return !queue_.empty() || !handler_.isRunning(); });
    if (!handler_.isRunning())
      break;

    // --- Now both conditions are true: connected and queue not empty ---
    while (!queue_.empty() && connected_.load() && handler_.isRunning()) {
      std::string payload = queue_.front();

      lock.unlock();
      int rc = mosquitto_publish(mosq_, nullptr, opt_c_str(cfg_.topic),
                                 payload.size(), payload.c_str(), 1, true);
      lock.lock();

      if (rc == MOSQ_ERR_SUCCESS) {
        queue_.pop();
        mqttLogger_->debug("Published MQTT message to topic '{}': {}",
                           cfg_.topic, payload);
      } else {
        mqttLogger_->error("MQTT publish failed: {}", mosquitto_strerror(rc));
        break;
      }
    }

    // Reset dropped count once queue is empty
    if (queue_.empty()) {
      droppedCount_ = 0;
    }
  }
  mqttLogger_->debug("MQTT client run loop stopped.");
}

// --- static callbacks ---

void MqttClient::onConnect(struct mosquitto *mosq, void *obj, int rc) {
  MqttClient *self = static_cast<MqttClient *>(obj);

  if (rc == 0) {
    self->connected_ = true;
    self->cv_.notify_one();
    self->mqttLogger_->info("MQTT connected successfully");
  } else {
    self->mqttLogger_->warn("MQTT connect failed: {} ({}), will retry...",
                            mosquitto_strerror(rc), rc);
  }
}

void MqttClient::onDisconnect(struct mosquitto *mosq, void *obj, int rc) {
  MqttClient *self = static_cast<MqttClient *>(obj);

  self->connected_ = false;

  if (rc == 0) {
    self->mqttLogger_->info("MQTT disconnected cleanly");
  } else {
    self->mqttLogger_->warn("MQTT connection failed: {} ({}), will retry...",
                            mosquitto_strerror(rc), rc);
  }
}

void MqttClient::onLog(struct mosquitto *mosq, void *obj, int level,
                       const char *str) {
  MqttClient *self = static_cast<MqttClient *>(obj);

  // Map mosquitto log levels to spdlog
  spdlog::level::level_enum spdLevel = spdlog::level::info;
  switch (level) {
  case MOSQ_LOG_DEBUG:
    spdLevel = spdlog::level::trace;
    break;
  case MOSQ_LOG_INFO:
  case MOSQ_LOG_NOTICE:
    spdLevel = spdlog::level::info;
    break;
  case MOSQ_LOG_WARNING:
    spdLevel = spdlog::level::warn;
    break;
  case MOSQ_LOG_ERR:
    spdLevel = spdlog::level::err;
    break;
  default:
    spdLevel = spdlog::level::info;
    break;
  }

  self->mqttLogger_->log(spdLevel, "mosquitto [{}]: {}", level, str);
}
