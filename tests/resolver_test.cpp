#include "resolver.hpp"
#include <arpa/inet.h>
#include <cassert>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

int main() {
    router::Config config;
    config.mappings.push_back({"up", "down", 2});
    std::atomic_bool stop{false};
    auto numeric = router::resolve_config(config, stop, router::Clock::now() + std::chrono::seconds(1));
    assert(numeric.upstream_host == "127.0.0.1");
    config.upstream_host = "localhost";
    auto local = router::resolve_config(config, stop, router::Clock::now() + std::chrono::seconds(1));
    assert(local.upstream_host == "127.0.0.1");
    // A real UDP DNS destination intentionally never responds. No global
    // resolver config or external network is touched by this failure fixture.
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    const auto server = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    config.upstream_host = "blackhole.invalid";
    for (bool cancel : {false, true}) {
        stop = false;
        std::thread cancellation;
        if (cancel) cancellation = std::thread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); stop = true; });
        const auto begin = router::Clock::now();
        bool rejected = false;
        try {
            router::resolve_config(config, stop, begin + (cancel ? std::chrono::seconds(3) : std::chrono::milliseconds(120)), server.c_str());
        } catch (const std::runtime_error&) { rejected = true; }
        if (cancellation.joinable()) cancellation.join();
        assert(rejected);
        assert(router::Clock::now() - begin < std::chrono::milliseconds(400));
    }
    close(fd);
    std::cout << "numeric, configured hostname, DNS deadline and cancellation passed\n";
}
