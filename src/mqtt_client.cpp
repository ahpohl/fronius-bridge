#include "mqtt_client.h"
#include "config_yaml.h"
#include "signal_handler.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <mosquitto.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

namespace {
// libmosquitto cert_reqs values for mosquitto_tls_opts_set, mirroring
// OpenSSL's SSL_VERIFY_NONE and SSL_VERIFY_PEER.
constexpr int tlsVerifyNone = 0;
constexpr int tlsVerifyPeer = 1;

// libmosquitto's tls_set() collapses a missing or unreadable file into an
// opaque INVAL; check first so the throw can name the path and the reason.
void requireReadable(const std::optional<std::string> &path, const char *what) {
  if (!path.has_value())
    return;
  if (::access(path->c_str(), R_OK) != 0)
    throw std::runtime_error(std::format("MQTT TLS: cannot read {} '{}': {}",
                                         what, *path, std::strerror(errno)));
}

// Network-loop poll timeout (ms). mosquitto_loop() wakes at least this often so
// the loop observes shutdown promptly and keepalive PINGs are sent on time.
constexpr int loopTimeoutMs = 100;

// MQTT 3.1.1 CONNACK "server unavailable" - the one refusal the broker can
// recover from on its own. mosquitto.h has no named constants for these codes.
constexpr int connackServerUnavailable = 3;

// Mirror mosquitto_loop_forever()'s own fatal/retryable split: it gives up (its
// thread returns) on these codes, and on MOSQ_ERR_ERRNO when errno is EPROTO -
// a protocol/TLS rejection that fails identically every retry, i.e. a
// misconfiguration. Everything else (refused, lost, no-conn) is retried. `err`
// must be the errno captured right after mosquitto_loop() returned. (A bad
// password is not MOSQ_ERR_AUTH - that is the unused v5 AUTH-packet flow; it
// arrives as a CONNACK refusal, handled in onConnect.)
bool isFatalLoopError(int rc, int err) {
  switch (rc) {
  case MOSQ_ERR_NOMEM:
  case MOSQ_ERR_PROTOCOL:
  case MOSQ_ERR_INVAL:
  case MOSQ_ERR_NOT_FOUND:
  case MOSQ_ERR_TLS:
  case MOSQ_ERR_PAYLOAD_SIZE:
  case MOSQ_ERR_NOT_SUPPORTED:
  case MOSQ_ERR_AUTH:
  case MOSQ_ERR_ACL_DENIED:
  case MOSQ_ERR_UNKNOWN:
  case MOSQ_ERR_EAI:
  case MOSQ_ERR_PROXY:
    return true;
  case MOSQ_ERR_ERRNO:
    return err == EPROTO;
  default:
    return false;
  }
}
} // namespace

MqttClient::MqttClient(const MqttConfig &cfg, SignalHandler &signalHandler)
    : cfg_(cfg), handler_(signalHandler) {

  // Setup mqtt logger
  logger_ = spdlog::get("mqtt");
  if (!logger_)
    logger_ = spdlog::default_logger();

  // Fail fast on unreadable TLS material before allocating any mosquitto
  // resources, so the throw needs no teardown and reports the exact path.
  if (cfg_.tls.has_value()) {
    const auto &tls = *cfg_.tls;
    requireReadable(tls.caFile, "ca_file");
    requireReadable(tls.caPath, "ca_path");
    requireReadable(tls.certFile, "cert_file");
    requireReadable(tls.keyFile, "key_file");
  }

  // Create Mosquitto client
  mosquitto_lib_init();
  mosq_ = mosquitto_new(nullptr, true, this);
  if (!mosq_) {
    throw std::runtime_error("Failed to create mosquitto client");
  }

  // Set username/password if provided
  if (cfg_.user.has_value()) {
    mosquitto_username_pw_set(mosq_, opt_c_str(cfg_.user),
                              opt_c_str(cfg_.password));
  }

  // Configure TLS if a tls block is present. All TLS options must be set on
  // the handle before connecting. With a CA file/path the broker certificate
  // is verified against it; with neither, fall back to the OS trust store so a
  // broker using a public CA (e.g. Let's Encrypt) connects without local certs.
  if (cfg_.tls.has_value()) {
    const auto &tls = *cfg_.tls;
    int rc = MOSQ_ERR_SUCCESS;

    if (tls.caFile.has_value() || tls.caPath.has_value()) {
      rc = mosquitto_tls_set(mosq_, opt_c_str(tls.caFile),
                             opt_c_str(tls.caPath), opt_c_str(tls.certFile),
                             opt_c_str(tls.keyFile), nullptr);
    } else {
      rc = mosquitto_int_option(mosq_, MOSQ_OPT_TLS_USE_OS_CERTS, 1);
    }
    if (rc != MOSQ_ERR_SUCCESS) {
      mosquitto_destroy(mosq_);
      mosq_ = nullptr;
      mosquitto_lib_cleanup();
      throw std::runtime_error(std::format(
          "Failed to configure MQTT TLS: {} ({})", mosquitto_strerror(rc), rc));
    }

    // Set advanced options only when something differs from libmosquitto's
    // defaults. insecure relaxes verification to SSL_VERIFY_NONE; otherwise the
    // default SSL_VERIFY_PEER stands.
    if (tls.tlsVersion.has_value() || tls.ciphers.has_value() || tls.insecure) {
      const int certReqs = tls.insecure ? tlsVerifyNone : tlsVerifyPeer;
      rc = mosquitto_tls_opts_set(mosq_, certReqs, opt_c_str(tls.tlsVersion),
                                  opt_c_str(tls.ciphers));
      if (rc != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        mosquitto_lib_cleanup();
        throw std::runtime_error(
            std::format("Failed to set MQTT TLS options: {} ({})",
                        mosquitto_strerror(rc), rc));
      }
    }

    // insecure also disables broker hostname verification, so a self-signed
    // certificate with a mismatched CN/SAN is accepted (testing only).
    if (tls.insecure)
      mosquitto_tls_insecure_set(mosq_, true);
  }

  // Set Mosquitto callbacks
  mosquitto_connect_callback_set(mosq_, MqttClient::onConnect);
  mosquitto_disconnect_callback_set(mosq_, MqttClient::onDisconnect);
  mosquitto_log_callback_set(mosq_, MqttClient::onLog);

  // We drive mosquitto_loop() ourselves and publish from the worker thread.
  // Without loop_start libmosquitto runs single-threaded, so mosquitto_publish()
  // would write inline and race the loop on the same TLS connection (bad record
  // mac); threaded_set defers writes to the loop thread, as loop_start did.
  mosquitto_threaded_set(mosq_, true);

  // Only initiate here; networkLoop() drives the handshake and all reconnects
  // (mosquitto_loop_start would quit on a TLS/protocol rejection). connect_async
  // just validates and queues, so a failure now is a real setup error.
  int rc = mosquitto_connect_async(mosq_, opt_c_str(cfg_.broker), cfg_.port, 60);
  if (rc != MOSQ_ERR_SUCCESS) {
    mosquitto_destroy(mosq_);
    mosq_ = nullptr;
    mosquitto_lib_cleanup();
    throw std::runtime_error(
        std::format("Failed to initiate MQTT connection: {} ({})",
                    mosquitto_strerror(rc), rc));
  }

  // Network I/O + reconnect and the publish queue run on their own threads.
  networkThread_ = std::thread(&MqttClient::networkLoop, this);
  worker_ = std::thread(&MqttClient::run, this);
}

MqttClient::~MqttClient() {
  cv_.notify_all();
  if (networkThread_.joinable())
    networkThread_.join();
  if (worker_.joinable())
    worker_.join();

  if (mosq_) {
    mosquitto_disconnect(mosq_);
    mosquitto_destroy(mosq_);
    mosq_ = nullptr;
  }

  mosquitto_lib_cleanup();
}

bool MqttClient::hasQueuedMessages() const {
  return std::any_of(topicQueues_.begin(), topicQueues_.end(),
                     [](const auto &p) { return !p.second.empty(); });
}

void MqttClient::publish(std::string payload, const std::string &topic) {
  std::unique_lock<std::mutex> lock(mutex_);

  // Duplicate suppression per topic
  std::size_t payloadHash = std::hash<std::string>{}(payload);
  auto itHash = lastPayloadHashes_.find(topic);
  if (itHash != lastPayloadHashes_.end() && itHash->second == payloadHash)
    return;
  lastPayloadHashes_[topic] = payloadHash;

  auto &q = topicQueues_[topic];

  // If topic queue is full, drop oldest for that topic
  if (q.size() >= cfg_.queueSize) {
    q.pop();
    droppedCount_[topic]++;
  }

  q.push({payload});

  // Logging only if disconnected
  if (!connected_.load()) {
    if (droppedCount_[topic] > 0) {
      logger_->warn("MQTT queue full for topic '{}', dropped oldest "
                    "message (total dropped: {})",
                    topic, droppedCount_[topic]);
    } else {
      if (q.size() > 0) {
        logger_->debug(
            "Waiting for MQTT connection... ({} messages cached for '{}')",
            q.size(), topic);
      }
    }
  }

  cv_.notify_one();
}

void MqttClient::networkLoop() {
  const std::chrono::seconds minDelay{cfg_.reconnectDelay.min};
  const std::chrono::seconds maxDelay{cfg_.reconnectDelay.max};
  const bool exponential = cfg_.reconnectDelay.exponential;

  std::chrono::seconds backoff{minDelay};

  while (handler_.isRunning()) {
    const int rc = mosquitto_loop(mosq_, loopTimeoutMs, 1);
    const int err = errno;

    if (rc == MOSQ_ERR_SUCCESS) {
      if (connected_.load())
        backoff = minDelay; // reset once a connection is actually established
      continue;
    }
    if (!handler_.isRunning())
      break;

    if (isFatalLoopError(rc, err)) {
      // Fatal: a misconfiguration that fails every retry; onLog already logged
      // any OpenSSL detail.
      handler_.shutdown(true, std::format("MQTT connection failed: {} ({})",
                                          mosquitto_strerror(rc), rc));
      return;
    }

    // Transient (broker down, refused, dropped): back off and re-arm, with the
    // same doubling schedule the other consumers use.
    logger_->warn("MQTT connection failed: {} ({}) - retrying (next wait {}s)",
                  mosquitto_strerror(rc), rc, backoff.count());
    {
      // Interruptible: the destructor's notify_all() wakes this on shutdown so
      // a long delay does not stall teardown.
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, backoff, [&] { return !handler_.isRunning(); });
    }
    backoff = exponential ? std::min(backoff * 2, maxDelay) : minDelay;
    if (!handler_.isRunning())
      break;

    mosquitto_reconnect_async(mosq_);
  }
}

void MqttClient::run() {
  while (handler_.isRunning()) {
    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [&] {
      return (connected_.load() && hasQueuedMessages()) ||
             !handler_.isRunning();
    });

    if (!handler_.isRunning()) {
      if (!connected_.load()) {
        break;
      }
      if (hasQueuedMessages()) {
        logger_->debug("Shutdown detected, flushing remaining messages");
      }
    }

    for (auto &[topic, q] : topicQueues_) {
      while (!q.empty() && connected_.load()) {
        auto payload = q.front().payload;
        lock.unlock();

        int rc = mosquitto_publish(mosq_, nullptr, opt_c_str(topic),
                                   payload.size(), payload.c_str(), 1, true);

        lock.lock();
        if (rc == MOSQ_ERR_SUCCESS) {
          q.pop();
          logger_->debug("Published MQTT message to topic '{}': {}", topic,
                         payload);
          droppedCount_[topic] = 0;
        } else {
          logger_->error("MQTT publish failed for '{}': {}", topic,
                         mosquitto_strerror(rc));
          break;
        }
      }
    }
  }

  logger_->debug("MQTT run loop stopped.");
}

void MqttClient::onConnect(struct mosquitto *mosq, void *obj, int rc) {
  MqttClient *self = static_cast<MqttClient *>(obj);

  self->connected_ = (rc == 0);
  self->cv_.notify_one();

  if (rc != 0) {
    // A negative CONNACK must be classified here: the drop that follows reaches
    // networkLoop only as a generic MOSQ_ERR_CONN_REFUSED. "Server unavailable"
    // can clear on its own (retry); any other refusal is a misconfiguration.
    if (rc == connackServerUnavailable) {
      self->logger_->warn("MQTT connection failed: {} ({})",
                          mosquitto_connack_string(rc), rc);
    } else {
      self->handler_.shutdown(
          true, std::format("MQTT connection failed: {} ({})",
                            mosquitto_connack_string(rc), rc));
    }
    return;
  }

  // mosquitto_ssl_get() yields the SSL handle on a TLS link, nullptr on a
  // plaintext one. Negotiated version and cipher go to debug; info just notes
  // whether the link is encrypted.
  if (SSL *ssl = static_cast<SSL *>(mosquitto_ssl_get(mosq))) {
    self->logger_->info("MQTT connected (TLS)");
    const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);
    self->logger_->debug("MQTT: {} negotiated, cipher {}", SSL_get_version(ssl),
                         cipher ? SSL_CIPHER_get_name(cipher) : "unknown");
  } else {
    self->logger_->info("MQTT connected");
  }
}

void MqttClient::onDisconnect(struct mosquitto *mosq, void *obj, int rc) {
  MqttClient *self = static_cast<MqttClient *>(obj);

  self->connected_ = false;

  // networkLoop() owns the classify/log/reconnect-or-shutdown decision, made
  // from mosquitto_loop()'s return where errno is still valid. Here we only
  // clear the flag; rc == 0 is our own disconnect at shutdown.
  if (rc == 0)
    self->logger_->info("MQTT disconnected");
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

  self->logger_->log(spdLevel, "mosquitto [{}]: {}", level, str);
}
