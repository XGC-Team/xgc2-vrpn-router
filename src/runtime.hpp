#pragma once
#include "router.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace router {
enum class NativeState { Starting, Ready, Failed, Stopped };
enum class ApplyState { Idle, Queued, Claimed, Applied, Expired, Conflict, Stopped };
struct Snapshot {
    NativeState state = NativeState::Starting;
    Clock::time_point source_time = Clock::now(), applied_at{};
    bool listening = false, upstream_okay = false, upstream_connected = false, downstream_connected = false;
    bool forwarding_enabled = true;
    double mainloop_rate_hz = 240;
    std::uint64_t applied_revision = 0;
    std::array<char, 129> applied_request_id{};
    std::array<RouteStats, max_mappings> routes{};
};
struct LocalUpdate {
    std::uint64_t expected_revision = 0;
    double rate = 0;
    bool enabled = false;
    Clock::time_point deadline{};
    std::array<char, 129> request_id{};
};

// Fixed native worker: the third-party VRPN implementation can block inside
// its own protocol IO. No VRPN object is ever touched by the HTTP owner. The
// one mailbox and fixed snapshot include all retained inter-thread work.
class Runtime {
  public:
    explicit Runtime(Config config);
    ~Runtime();
    void start(std::function<void()> wake);
    Snapshot snapshot() const;
    Config applied_config(const Snapshot& snapshot) const;
    const Config& initial_config() const noexcept { return config_; }
    ApplyState submit(LocalUpdate update);
    ApplyState completion() const;
    void release_completion();
    void request_stop() noexcept;
    bool done() const noexcept { return done_.load(); }
    bool wait_until(Clock::time_point deadline);
    void join();

  private:
    void work() noexcept;
    void publish(const Router& router);
    Config config_;
    mutable std::mutex mutex_;
    std::condition_variable finished_;
    Snapshot snapshot_;
    LocalUpdate update_;
    ApplyState apply_state_ = ApplyState::Idle;
    std::atomic_bool stop_{false}, done_{false};
    std::thread worker_;
    std::function<void()> wake_;
};
} // namespace router
