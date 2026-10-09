#include "runtime.hpp"
#include "resolver.hpp"
#include <cstring>
#include <limits>

namespace router {
Runtime::Runtime(Config config) : config_(std::move(config)) { validate(config_); }
Runtime::~Runtime() {
    // The application must retain this object and the endpoint lease until
    // done. A blocked native worker requires explicit process termination;
    // destroying its state or detaching it would be unsafe.
    if (worker_.joinable())
        std::terminate();
}
void Runtime::start(std::function<void()> wake) {
    if (worker_.joinable())
        throw std::logic_error("native worker already started");
    wake_ = std::move(wake);
    worker_ = std::thread([this] { work(); });
}
Snapshot Runtime::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}
Config Runtime::applied_config(const Snapshot& snapshot) const {
    auto config = config_;
    config.mainloop_rate_hz = snapshot.mainloop_rate_hz;
    config.forwarding_enabled = snapshot.forwarding_enabled;
    return config;
}
ApplyState Runtime::submit(LocalUpdate update) {
    std::lock_guard lock(mutex_);
    if (stop_ || snapshot_.state != NativeState::Ready)
        return ApplyState::Stopped;
    if (apply_state_ != ApplyState::Idle)
        return apply_state_;
    if (update.expected_revision != snapshot_.applied_revision)
        return ApplyState::Conflict;
    update_ = update;
    apply_state_ = ApplyState::Queued;
    return ApplyState::Idle; // Only admission; no application is claimed here.
}
ApplyState Runtime::completion() const {
    std::lock_guard lock(mutex_);
    return apply_state_;
}
void Runtime::release_completion() {
    std::lock_guard lock(mutex_);
    if (apply_state_ != ApplyState::Queued && apply_state_ != ApplyState::Claimed)
        apply_state_ = ApplyState::Idle;
}
void Runtime::request_stop() noexcept { stop_.store(true); }
bool Runtime::wait_until(Clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    return finished_.wait_until(lock, deadline, [&] { return done(); });
}
void Runtime::join() {
    if (!done())
        throw std::logic_error("native worker is not quiescent");
    if (worker_.joinable())
        worker_.join();
}
void Runtime::publish(const Router& router) {
    std::lock_guard lock(mutex_);
    snapshot_.source_time = Clock::now();
    snapshot_.listening = router.listening();
    snapshot_.upstream_okay = router.upstream_okay();
    snapshot_.upstream_connected = router.upstream_connected();
    snapshot_.downstream_connected = router.downstream_connected();
    for (std::size_t i = 0; i < config_.mappings.size(); ++i)
        snapshot_.routes[i] = router.stats(i);
}
void Runtime::work() noexcept {
    try {
        {
            // Construction, all protocol calls and destruction are confined
            // here: native connection objects are explicitly not thread safe.
            const auto startup_deadline = Clock::now() + std::chrono::seconds(5);
            Router router(resolve_config(config_, stop_, startup_deadline));
            while (!stop_) {
                router.tick();
                publish(router);
                if (router.listening() && router.upstream_connected())
                    break;
                if (Clock::now() >= startup_deadline)
                    throw std::runtime_error("native upstream connect deadline expired");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (stop_)
                throw std::runtime_error("native startup cancelled");
            {
                std::lock_guard lock(mutex_);
                snapshot_.state = NativeState::Ready;
                snapshot_.applied_revision = 1;
                snapshot_.applied_at = Clock::now();
                snapshot_.mainloop_rate_hz = config_.mainloop_rate_hz;
                snapshot_.forwarding_enabled = config_.forwarding_enabled;
            }
            wake_();
            while (!stop_) {
                const auto begin = Clock::now();
                LocalUpdate update;
                bool claimed = false;
                {
                    std::lock_guard lock(mutex_);
                    if (apply_state_ == ApplyState::Queued) {
                        update = update_;
                        apply_state_ = ApplyState::Claimed;
                        claimed = true;
                    }
                }
                if (claimed) {
                    ApplyState outcome;
                    {
                        std::lock_guard lock(mutex_);
                        // This is the actual local apply boundary: lock wait
                        // consumes the caller budget, just as mailbox wait did.
                        if (Clock::now() >= update.deadline || stop_)
                            outcome = ApplyState::Expired;
                        else if (update.expected_revision != snapshot_.applied_revision ||
                                 snapshot_.applied_revision == std::numeric_limits<std::uint64_t>::max())
                            outcome = ApplyState::Conflict;
                        else {
                            router.apply_local(update.rate, update.enabled);
                            snapshot_.mainloop_rate_hz = update.rate;
                            snapshot_.forwarding_enabled = update.enabled;
                            ++snapshot_.applied_revision;
                            snapshot_.applied_at = Clock::now();
                            snapshot_.applied_request_id = update.request_id;
                            outcome = ApplyState::Applied;
                        }
                    }
                    {
                        std::lock_guard lock(mutex_);
                        apply_state_ = outcome;
                    }
                    wake_();
                }
                router.tick();
                publish(router);
                const auto period = std::chrono::duration<double>(1 / router.config().mainloop_rate_hz);
                std::this_thread::sleep_until(begin + std::chrono::duration_cast<Clock::duration>(period));
            }
        } // Native destructor can itself block; done is published only after it.
        std::lock_guard lock(mutex_);
        snapshot_.state = NativeState::Stopped;
        if (apply_state_ == ApplyState::Queued || apply_state_ == ApplyState::Claimed)
            apply_state_ = ApplyState::Stopped;
    } catch (...) {
        std::lock_guard lock(mutex_);
        snapshot_.state = NativeState::Failed;
        apply_state_ = ApplyState::Stopped;
    }
    {
        std::lock_guard lock(mutex_);
        done_.store(true);
    }
    finished_.notify_all();
    wake_();
}
} // namespace router
