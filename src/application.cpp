#include "application.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace router {
namespace {
std::atomic_bool stopping{false};
static_assert(std::atomic_bool::is_always_lock_free);
void stop(int) { stopping.store(true, std::memory_order_relaxed); }
struct OwnedFd {
    int value;
    explicit OwnedFd(int fd) : value(fd) {
        if (fd < 0)
            throw std::invalid_argument("explicit runtime directory allocation required");
    }
    ~OwnedFd() { ::close(value); }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
};
Json::Value reference_json(const xgc2::xrpc::ServiceRef& ref) {
    Json::Value value;
    value["target_id"] = ref.target_id;
    value["service"] = ref.service;
    value["api_version"] = ref.api_version;
    value["profile"] = ref.profile;
    value["instance_id"] = ref.instance_id;
    value["endpoint"]["kind"] = ref.endpoint.kind;
    value["endpoint"]["address"] = ref.endpoint.address;
    return value;
}
bool authorization_name(std::string_view name) {
    constexpr std::string_view expected = "authorization";
    return name.size() == expected.size() &&
           std::equal(name.begin(), name.end(), expected.begin(),
                      [](unsigned char a, unsigned char b) { return std::tolower(a) == b; });
}
} // namespace

int run(Config config, const xgc2::xrpc::BootstrapInput& bootstrap, const xgc2::xrpc::RuntimePolicy& policy) {
    const auto& binding = bootstrap.binding();
    if (binding.service() != "xgc2.vrpn-router" || binding.api_version() != "1" || binding.profile() != "http.v1" ||
        binding.endpoint().kind != "unix" || binding.authentication() != "local_private" ||
        !binding.storage_grants().empty())
        throw std::invalid_argument("local-private VRPN router HTTP v1 binding required");
    const auto runtime = bootstrap.resolve_runtime([](const auto&, const auto& owner) {
        const auto& path = owner.endpoint().address;
        OwnedFd directory(
            ::open(path.substr(0, path.rfind('/')).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        return xgc2::xrpc::DirectoryGrant::from_owned_directory(directory.value, xgc2::xrpc::GrantPurpose::Runtime);
    });
    OwnedFd retained_parent(runtime.duplicate_fd());
    xgc2::xrpc::UnixOptions endpoint;
    endpoint.path = binding.endpoint().address;
    endpoint.existing = xgc2::xrpc::ExistingPath::ReclaimUnreachable;
    const auto reference = binding.service_ref(xgc2::xrpc::new_instance_id());
    stopping.store(false, std::memory_order_relaxed);
    struct sigaction action {};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) || sigaction(SIGTERM, &action, nullptr))
        throw std::runtime_error("signal handler installation failed");
    xgc2::xrpc::Diagnostics diagnostics(policy);
    Runtime router(std::move(config));
    Service service(router, reference_json(reference), policy, diagnostics);
    xgc2::xrpc::HttpServer host(
        endpoint,
        [&](auto request, auto reply) {
            std::array<std::string_view, 2> authorization;
            std::size_t count = 0;
            for (const auto& [name, value] : request.headers) {
                if (!authorization_name(name))
                    continue;
                if (count == authorization.size())
                    break;
                authorization[count++] = value;
            }
            if (!bootstrap.authorize(std::span(authorization).first(count), request.deadline)) {
                reply.complete(xgc2::xrpc::http_error(401, "unauthenticated", "authorization rejected"));
                return;
            }
            service.dispatch(std::move(request), std::move(reply));
        },
        xgc2::xrpc::http_limits(policy), {reference.instance_id, {"/v1/describe"}}, retained_parent.value);
    service.set_host(host);
    host.set_diagnostics(&diagnostics, "xgc2.vrpn-router");
    host.set_wakeup_handler([&] { service.complete_native_update(); });
    router.start([&] { host.wake(); });
    try {
        while (!stopping.load(std::memory_order_relaxed) && !router.done()) {
            // Asio continues serving requests/timers throughout this wait. Native
            // protocol IO never runs on this owner. The outer bound is solely for
            // observing the process signal flag without installing another socket
            // or signal-handler thread.
            host.poll(std::chrono::seconds(1));
        }
        service.begin_drain();
        router.request_stop();
        const auto deadline = Clock::now() + std::chrono::milliseconds(policy.integer("SHUTDOWN_TIMEOUT_MS"));
        while (!router.done() && Clock::now() < deadline) {
            host.poll(std::min(std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()),
                               std::chrono::milliseconds(100)));
        }
        if (!router.done()) {
            // No C++ object/lease destruction while a native call is still using
            // them. Exit 2 is failed native quiescence, never graceful completion.
            // The OS terminates this process and releases all native/SDK resources
            // together. A supervisor can then restart using a fresh instance ID.
            std::_Exit(2);
        }
        router.join();
        service.complete_native_update();
        const bool failed = router.snapshot().state == NativeState::Failed;
        return host.drain() ? (failed ? 1 : 0) : 2;
    } catch (...) {
        // Owner callbacks can allocate or throw. Never unwind/destroy host
        // while the worker can still use its wake FD or native records.
        router.request_stop();
        if (!router.wait_until(Clock::now() + std::chrono::milliseconds(policy.integer("SHUTDOWN_TIMEOUT_MS"))))
            std::_Exit(2);
        router.join();
        throw;
    }
}
} // namespace router
