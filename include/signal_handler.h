#ifndef SIGNAL_HANDLER_H_
#define SIGNAL_HANDLER_H_

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <string>

class SignalHandler {
public:
  explicit SignalHandler(void) : running_(true) {
    struct sigaction action{};
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = [](int sig, siginfo_t *, void *) {
      if (instance_ && instance_->running_.load()) {
        instance_->signal_ = sig;
        instance_->shutdown();
      }
    };
    sigemptyset(&action.sa_mask);
    instance_ = this;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
  }

  ~SignalHandler() {
    struct sigaction defaultAction{};
    defaultAction.sa_handler = SIG_DFL;
    sigaction(SIGINT, &defaultAction, nullptr);
    sigaction(SIGTERM, &defaultAction, nullptr);
    instance_ = nullptr;
  }

  // --- Delete copy and assignment ---
  SignalHandler(const SignalHandler &) = delete;
  SignalHandler &operator=(const SignalHandler &) = delete;

  // --- Programmatic shutdown ---
  // `failure` marks a non-recoverable error (vs. a signal or clean stop) so the
  // process exits with a failure status; `reason` is the message main reports,
  // mirroring signalName() for the signal case. The signal handler calls the
  // no-arg form, keeping signal-driven shutdowns successful.
  void shutdown(bool failure = false, std::string reason = {}) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (failure) {
        failed_.store(true);
        reason_ = std::move(reason);
      }
      running_.store(false);
    }
    cv_.notify_all();
  }

  // --- Wait for shutdown ---
  void wait() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [&] { return !running_.load(); });
  }

  const char *signalName() const { return strsignal(signal_); }

  int signal() const { return signal_; }
  bool isRunning() const { return running_.load(); }
  bool failed() const { return failed_.load(); }
  const std::string &reason() const { return reason_; }

private:
  std::atomic<bool> running_;
  std::atomic<bool> failed_{false};
  std::string reason_;
  std::mutex mtx_;
  std::condition_variable cv_;
  static inline SignalHandler *instance_ = nullptr;
  std::atomic<int> signal_{0};
};

#endif /* SIGNAL_HANDLER_H_ */
