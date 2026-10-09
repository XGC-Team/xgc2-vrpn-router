#include <vrpn_Tracker.h>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
struct Release { void operator()(vrpn_Connection* connection) const { if (connection) connection->removeReference(); } };
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
constexpr timeval stamp{123, 456789};
constexpr vrpn_float64 position[]{1, 2, 3}, rotation[]{0, 0, 0.6, 0.8};
constexpr vrpn_float64 velocity[]{4, 5, 6}, acceleration[]{7, 8, 9};
struct Received {
    int pose = 0, velocity = 0, acceleration = 0, invalid = 0;
};
bool time_ok(timeval value) { return value.tv_sec == stamp.tv_sec && value.tv_usec == stamp.tv_usec; }
bool vector_ok(const double* a, const double* b, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
void VRPN_CALLBACK pose(void* data, vrpn_TRACKERCB value) {
    auto& counts = *static_cast<Received*>(data);
    ++counts.pose;
    if (!time_ok(value.msg_time) || value.sensor != 1 || !vector_ok(value.pos, position, 3) ||
        !vector_ok(value.quat, rotation, 4))
        ++counts.invalid;
}
void VRPN_CALLBACK vel(void* data, vrpn_TRACKERVELCB value) {
    auto& counts = *static_cast<Received*>(data);
    ++counts.velocity;
    if (!time_ok(value.msg_time) || value.sensor != 1 || !vector_ok(value.vel, velocity, 3) ||
        !vector_ok(value.vel_quat, rotation, 4) || value.vel_quat_dt != 0.25)
        ++counts.invalid;
}
void VRPN_CALLBACK acc(void* data, vrpn_TRACKERACCCB value) {
    auto& counts = *static_cast<Received*>(data);
    ++counts.acceleration;
    if (!time_ok(value.msg_time) || value.sensor != 1 || !vector_ok(value.acc, acceleration, 3) ||
        !vector_ok(value.acc_quat, rotation, 4) || value.acc_quat_dt != 0.5)
        ++counts.invalid;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    const bool source = std::string(argv[1]) == "source";
    std::unique_ptr<vrpn_Connection, Release> connection(source ? vrpn_create_server_connection(argv[2])
                                         : vrpn_get_connection_by_name(argv[2]));
    if (!connection || !connection->doing_okay())
        return 3;
    if (source) {
        vrpn_Tracker_Server tracker("up", connection.get(), 2);
        while (!stopping) {
            tracker.mainloop();
            tracker.report_pose(1, stamp, position, rotation);
            tracker.report_pose_velocity(1, stamp, velocity, rotation, 0.25);
            tracker.report_pose_acceleration(1, stamp, acceleration, rotation, 0.5);
            connection->mainloop();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }
    vrpn_Tracker_Remote tracker("down", connection.get());
    Received received;
    tracker.register_change_handler(&received, pose);
    tracker.register_change_handler(&received, vel);
    tracker.register_change_handler(&received, acc);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!stopping && Clock::now() < deadline &&
           (received.pose < 10 || received.velocity < 10 || received.acceleration < 10)) {
        tracker.mainloop();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cout << "{\"pose\":" << received.pose << ",\"velocity\":" << received.velocity
              << ",\"acceleration\":" << received.acceleration << ",\"invalid\":" << received.invalid << "}\n";
    return received.pose >= 10 && received.velocity >= 10 && received.acceleration >= 10 && !received.invalid ? 0 : 4;
}
