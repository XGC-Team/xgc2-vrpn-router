#include "config.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

unsigned rejection_case = 0;
template <typename Function> void rejected(Function function) {
    ++rejection_case;
    bool failed = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    if (!failed) {
        std::cerr << "rejection case " << rejection_case << " was accepted\n";
        std::abort();
    }
}

int main() {
    router::Config original;
    original.mappings = {{"up", "down", 2}};
    const auto document = router::config_json(original);
    auto candidate = document;
    candidate["listen_port"] = 65536;
    rejected([&] { router::parse_config(candidate); });
    candidate = document;
    candidate["mainloop_rate_hz"] = Json::Value();
    rejected([&] { router::parse_config(candidate); });
    candidate = document;
    candidate["mappings"].append(candidate["mappings"][0]);
    rejected([&] { router::parse_config(candidate); });
    candidate = document;
    candidate["mappings"][0]["upstream"] = "file:///tmp/should-not-be-opened";
    rejected([&] { router::parse_config(candidate); });
    candidate = document;
    candidate["upstream_host"] = "file:///tmp/should-not-be-opened";
    rejected([&] { router::parse_config(candidate); });
    candidate = document;
    candidate.removeMember("forwarding_enabled");
    rejected([&] { router::parse_config(candidate); });
    candidate = document;
    candidate["schema_version"] = 2;
    rejected([&] { router::parse_config(candidate); });
    candidate = document;
    candidate["mappings"] = Json::Value(Json::arrayValue);
    for (unsigned i = 0; i < router::max_mappings; ++i) {
        auto entry = document["mappings"][0];
        entry["downstream"] = "down_" + std::to_string(i);
        candidate["mappings"].append(std::move(entry));
    }
    assert(router::parse_config(candidate).mappings.size() == router::max_mappings);
    candidate["mappings"].append(document["mappings"][0]);
    rejected([&] { router::parse_config(candidate); });
    for (const auto* invalid :
         {"{\"a\":1,\"a\":2}", "{} {}", "{/*comment*/}", "{\"a\":NaN}", "{\"a\":1,}", "/*comment*/{}", "{}/*comment*/",
          "{\"a\":1,/*comment*/\"b\":2}", "{\"a\":{}/*comment*/}", "{\"a\":[/*comment*/]}"})
        rejected([&] { router::parse_json(invalid); });
    original.mainloop_rate_hz = INFINITY;
    rejected([&] { router::validate(original); });
    std::cout << "bounded schema, no fallback, duplicate keys and native URL rejection passed\n";
}
