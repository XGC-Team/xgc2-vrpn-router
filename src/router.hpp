#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vrpn_Connection.h>
#include <vrpn_Tracker.h>

namespace router {
using Clock = std::chrono::steady_clock;
inline constexpr std::size_t max_mappings = 128;
inline constexpr std::size_t max_config_bytes = 65536;
inline constexpr unsigned native_messages_per_tick = 256;

struct Mapping {
    std::string upstream;
    std::string downstream;
    int sensors = 1;
};
struct Config {
    std::string upstream_host = "127.0.0.1";
    std::uint16_t upstream_port = 3883;
    std::string bind_address = "127.0.0.1";
    std::uint16_t listen_port = 3884;
    double mainloop_rate_hz = 240;
    double upstream_update_rate_hz = 0;
    bool forwarding_enabled = true;
    std::vector<Mapping> mappings;
};
void validate(const Config& config);

struct SampleStats {
    std::uint64_t received = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t send_failed = 0;
    Clock::time_point last_received{};
};
struct RouteStats {
    SampleStats pose, velocity, acceleration;
    std::uint64_t invalid_sensor = 0;
};
struct ConnectionRelease {
    void operator()(vrpn_Connection* connection) const noexcept;
};
using Connection = std::unique_ptr<vrpn_Connection, ConnectionRelease>;

// A fixed set of native connections and contiguous route records. All methods
// belong to the application's single owner thread; this is a best-effort
// network forwarder, not a hard realtime control loop.
class Router {
  public:
    explicit Router(Config config);
    void tick();
    void apply_local(double mainloop_rate_hz, bool forwarding_enabled);
    const Config& config() const noexcept { return config_; }
    bool listening() const noexcept;
    bool upstream_okay() const noexcept;
    bool upstream_connected() const noexcept;
    bool downstream_connected() const noexcept;
    Clock::time_point source_time() const noexcept { return source_time_; }
    const RouteStats& stats(std::size_t index) const { return routes_.at(index).stats; }

  private:
    struct Route {
        int sensors = 0;
        bool enabled = true;
        std::unique_ptr<vrpn_Tracker_Server> server;
        std::unique_ptr<vrpn_Tracker_Remote> remote;
        RouteStats stats;
    };
    static bool accept(Route& route, vrpn_int32 sensor, SampleStats& stats);
    static void VRPN_CALLBACK pose(void*, vrpn_TRACKERCB);
    static void VRPN_CALLBACK velocity(void*, vrpn_TRACKERVELCB);
    static void VRPN_CALLBACK acceleration(void*, vrpn_TRACKERACCCB);

    Config config_;
    Connection server_;
    Connection upstream_;
    // No reallocation/removal after callbacks receive addresses into this vector.
    std::vector<Route> routes_;
    Clock::time_point source_time_{};
};
} // namespace router
