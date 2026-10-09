#include "resolver.hpp"
#include <algorithm>
#include <ares.h>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <poll.h>
#include <stdexcept>

namespace router {
namespace {
struct Resolver {
    ares_channel_t* channel = nullptr;
    std::array<pollfd, 16> sockets{};
    bool done = false, overflow = false;
    int status = ARES_ECANCELLED;
    std::array<char, INET_ADDRSTRLEN> address{};
    Resolver() {
        for (auto& fd : sockets)
            fd.fd = -1;
        const auto result = ares_library_init(ARES_LIB_INIT_ALL);
        if (result != ARES_SUCCESS)
            throw std::runtime_error("DNS library initialization failed");
    }
    ~Resolver() {
        if (channel) {
            ares_cancel(channel);
            ares_destroy(channel);
        }
        ares_library_cleanup();
    }
    static void socket_state(void* data, ares_socket_t fd, int readable, int writable) noexcept {
        auto& self = *static_cast<Resolver*>(data);
        auto found = std::find_if(self.sockets.begin(), self.sockets.end(), [fd](auto& p) { return p.fd == fd; });
        if (found == self.sockets.end()) {
            if (!readable && !writable)
                return;
            found = std::find_if(self.sockets.begin(), self.sockets.end(), [](auto& p) { return p.fd < 0; });
        }
        if (found == self.sockets.end()) {
            self.overflow = true;
            return;
        }
        *found = pollfd{readable || writable ? fd : -1,
                        static_cast<short>((readable ? POLLIN : 0) | (writable ? POLLOUT : 0)), 0};
    }
    static void completed(void* data, int status, int, ares_addrinfo* result) noexcept {
        auto& self = *static_cast<Resolver*>(data);
        self.status = status;
        if (status == ARES_SUCCESS) {
            self.status = ARES_ENODATA;
            for (auto* node = result->nodes; node; node = node->ai_next) {
                if (node->ai_family == AF_INET && node->ai_addrlen >= sizeof(sockaddr_in) &&
                    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(node->ai_addr)->sin_addr, self.address.data(),
                              self.address.size())) {
                    self.status = ARES_SUCCESS;
                    break;
                }
            }
        }
        ares_freeaddrinfo(result);
        self.done = true;
    }
    std::string resolve(const std::string& host, const std::atomic_bool& stop, Clock::time_point deadline) {
        in_addr numeric{};
        if (host.empty() || inet_pton(AF_INET, host.c_str(), &numeric) == 1)
            return host;
        if (stop || Clock::now() >= deadline)
            throw std::runtime_error("DNS resolution cancelled or expired");
        done = false;
        ares_addrinfo_hints hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = ARES_AI_NOSORT; // Sorting otherwise probes target routes.
        ares_getaddrinfo(channel, host.c_str(), nullptr, &hints, completed, this);
        while (!done && !stop && !overflow && Clock::now() < deadline) {
            auto descriptors = sockets;
            // This is configuration work only. A 50 ms maximum wait makes an
            // explicit process cancellation bounded without a resolver thread.
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
            const int count =
                poll(descriptors.data(), descriptors.size(), static_cast<int>(std::clamp<long long>(left, 0, 50)));
            if (count < 0 && errno != EINTR)
                throw std::runtime_error("DNS event wait failed");
            std::array<ares_fd_events_t, 16> events{};
            std::size_t used = 0;
            for (const auto& fd : descriptors) {
                if (fd.fd < 0 || !fd.revents)
                    continue;
                unsigned flags = 0;
                if (fd.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL))
                    flags |= ARES_FD_EVENT_READ;
                if (fd.revents & POLLOUT)
                    flags |= ARES_FD_EVENT_WRITE;
                events[used++] = {fd.fd, flags};
            }
            if (ares_process_fds(channel, events.data(), used, ARES_PROCESS_FLAG_NONE) != ARES_SUCCESS)
                throw std::runtime_error("DNS event processing failed");
        }
        if (!done) {
            ares_cancel(channel);
            throw std::runtime_error("DNS resolution cancelled, expired or capacity exceeded");
        }
        if (status != ARES_SUCCESS)
            throw std::runtime_error("configured hostname did not resolve to IPv4");
        return address.data();
    }
};
} // namespace
Config resolve_config(const Config& input, const std::atomic_bool& stop, Clock::time_point deadline,
                      const char* test_dns_server) {
    validate(input);
    Resolver resolver;
    ares_options options{};
    options.timeout = 1000;
    options.tries = 2;
    options.sock_state_cb = Resolver::socket_state;
    options.sock_state_cb_data = &resolver;
    // c-ares owns parsing/resolution. No event thread or implicit background refresh.
    if (ares_init_options(&resolver.channel, &options, ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES | ARES_OPT_SOCK_STATE_CB) !=
        ARES_SUCCESS)
        throw std::runtime_error("DNS channel initialization failed");
    if (test_dns_server && ares_set_servers_ports_csv(resolver.channel, test_dns_server) != ARES_SUCCESS)
        throw std::runtime_error("test DNS server configuration failed");
    auto output = input;
    output.upstream_host = resolver.resolve(input.upstream_host, stop, deadline);
    output.bind_address = resolver.resolve(input.bind_address, stop, deadline);
    return output;
}
} // namespace router
