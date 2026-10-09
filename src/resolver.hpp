#pragma once
#include "router.hpp"
#include <atomic>

namespace router {
// One bounded, cancellable configuration-time lookup. No DNS runs inside VRPN
// callbacks or against names supplied by an untrusted native peer.
Config resolve_config(const Config&, const std::atomic_bool& stop,
                      Clock::time_point deadline, const char* test_dns_server = nullptr);
}
