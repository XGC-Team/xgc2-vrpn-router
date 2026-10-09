#include "router.hpp"

#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace router {
namespace {
bool name_valid(const std::string& name) {
    if (name.empty() || name.size() > 63)
        return false;
    for (const unsigned char c : name)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_' && c != '-' &&
            c != '.')
            return false;
    return true;
}
bool host_valid(const std::string& host, bool allow_empty) {
    if (host.empty())
        return allow_empty;
    if (host.size() > 253)
        return false;
    for (const unsigned char c : host)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '-' && c != '.')
            return false;
    return true;
}
} // namespace

void validate(const Config& config) {
    if (!host_valid(config.upstream_host, false) || !host_valid(config.bind_address, true))
        throw std::invalid_argument("host/address must be a bounded IPv4 address or DNS name, without a port or URL");
    if (!config.upstream_port || !config.listen_port)
        throw std::invalid_argument("ports must be in 1..65535");
    if (!std::isfinite(config.mainloop_rate_hz) || config.mainloop_rate_hz < 1 || config.mainloop_rate_hz > 2000)
        throw std::invalid_argument("mainloop_rate_hz must be in 1..2000");
    if (!std::isfinite(config.upstream_update_rate_hz) || config.upstream_update_rate_hz < 0 ||
        config.upstream_update_rate_hz > 2000)
        throw std::invalid_argument("upstream_update_rate_hz must be in 0..2000");
    if (config.mappings.empty() || config.mappings.size() > max_mappings)
        throw std::invalid_argument("mappings count must be in 1..128");
    std::unordered_set<std::string> downstream;
    for (const auto& mapping : config.mappings) {
        if (!name_valid(mapping.upstream) || !name_valid(mapping.downstream))
            throw std::invalid_argument("tracker names must be 1..63 ASCII letters, digits, dot, dash or underscore");
        if (mapping.sensors < 1 || mapping.sensors > 256)
            throw std::invalid_argument("mapping sensors must be in 1..256");
        if (!downstream.insert(mapping.downstream).second)
            throw std::invalid_argument("duplicate downstream tracker name");
    }
}

void ConnectionRelease::operator()(vrpn_Connection* connection) const noexcept {
    if (connection)
        connection->removeReference();
}

Router::Router(Config config) : config_(std::move(config)) {
    static_assert(VRPN_XGC_NATIVE_PROFILE == 20261009);
    if (vrpn_xgc_native_profile() != VRPN_XGC_NATIVE_PROFILE)
        throw std::runtime_error("unapproved native VRPN dependency profile");
    // Remote text is not a product log grant. Use the native opt-out before
    // objects register the default text printer's decoder/callbacks.
    vrpn_System_TextPrinter.set_ostream_to_use(nullptr);
    validate(config_);
    const auto listen = config_.bind_address + ":" + std::to_string(config_.listen_port);
    server_.reset(vrpn_create_server_connection(listen.c_str()));
    // VRPN can return a non-null BROKEN connection when TCP/UDP bind fails.
    if (!server_ || !server_->doing_okay())
        throw std::runtime_error("native VRPN listener failed");
    const auto upstream = config_.upstream_host + ":" + std::to_string(config_.upstream_port);
    upstream_.reset(vrpn_get_connection_by_name(upstream.c_str()));
    if (!upstream_ || !upstream_->doing_okay())
        throw std::runtime_error("native VRPN upstream initialization failed");
    server_->Jane_stop_this_crazy_thing(native_messages_per_tick);
    upstream_->Jane_stop_this_crazy_thing(native_messages_per_tick);
    routes_.resize(config_.mappings.size());
    for (std::size_t i = 0; i < routes_.size(); ++i) {
        auto& route = routes_[i];
        const auto& mapping = config_.mappings[i];
        route.sensors = mapping.sensors;
        route.enabled = config_.forwarding_enabled;
        route.server =
            std::make_unique<vrpn_Tracker_Server>(mapping.downstream.c_str(), server_.get(), mapping.sensors);
        route.remote = std::make_unique<vrpn_Tracker_Remote>(mapping.upstream.c_str(), upstream_.get());
        if (route.remote->register_change_handler(&route, &Router::pose) ||
            route.remote->register_change_handler(&route, &Router::velocity) ||
            route.remote->register_change_handler(&route, &Router::acceleration))
            throw std::runtime_error("native VRPN callback registration failed");
        if (config_.upstream_update_rate_hz > 0 && route.remote->set_update_rate(config_.upstream_update_rate_hz))
            throw std::runtime_error("native VRPN update-rate request failed");
    }
}

void Router::tick() {
    // Preserve native client/server mainloops, including VRPN ping/pong and
    // server handler initialization. Connections and execution remain shared.
    for (auto& route : routes_) {
        route.remote->mainloop();
        route.server->mainloop();
    }
    server_->mainloop();
    source_time_ = Clock::now();
}

void Router::apply_local(double rate, bool enabled) {
    if (!std::isfinite(rate) || rate < 1 || rate > 2000)
        throw std::invalid_argument("mainloop_rate_hz must be in 1..2000");
    config_.mainloop_rate_hz = rate;
    config_.forwarding_enabled = enabled;
    for (auto& route : routes_)
        route.enabled = enabled;
}
bool Router::listening() const noexcept { return server_->doing_okay(); }
bool Router::upstream_okay() const noexcept { return upstream_->doing_okay(); }
bool Router::upstream_connected() const noexcept { return upstream_->connected(); }
bool Router::downstream_connected() const noexcept { return server_->connected(); }

bool Router::accept(Route& route, vrpn_int32 sensor, SampleStats& stats) {
    if (sensor < 0 || sensor >= route.sensors) {
        ++route.stats.invalid_sensor;
        return false;
    }
    ++stats.received;
    stats.last_received = Clock::now();
    return route.enabled;
}
void VRPN_CALLBACK Router::pose(void* userdata, const vrpn_TRACKERCB info) {
    auto& route = *static_cast<Route*>(userdata);
    auto& stats = route.stats.pose;
    if (!accept(route, info.sensor, stats))
        return;
    if (route.server->report_pose(info.sensor, info.msg_time, info.pos, info.quat) == 0)
        ++stats.forwarded;
    else
        ++stats.send_failed;
}
void VRPN_CALLBACK Router::velocity(void* userdata, const vrpn_TRACKERVELCB info) {
    auto& route = *static_cast<Route*>(userdata);
    auto& stats = route.stats.velocity;
    if (!accept(route, info.sensor, stats))
        return;
    if (route.server->report_pose_velocity(info.sensor, info.msg_time, info.vel, info.vel_quat, info.vel_quat_dt) == 0)
        ++stats.forwarded;
    else
        ++stats.send_failed;
}
void VRPN_CALLBACK Router::acceleration(void* userdata, const vrpn_TRACKERACCCB info) {
    auto& route = *static_cast<Route*>(userdata);
    auto& stats = route.stats.acceleration;
    if (!accept(route, info.sensor, stats))
        return;
    if (route.server->report_pose_acceleration(info.sensor, info.msg_time, info.acc, info.acc_quat, info.acc_quat_dt) ==
        0)
        ++stats.forwarded;
    else
        ++stats.send_failed;
}
} // namespace router
