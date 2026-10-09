#include "application.hpp"
#include <iostream>

extern char** environ;

int main(int argc, char** argv) {
    try {
        std::string bootstrap_path;
        bool check = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "xgc2-vrpn-router --bootstrap-input INPUT\n"
                             "  --check-config validates the application JSON and exits\n";
                return 0;
            }
            if (option == "--version") {
                std::cout << "xgc2-vrpn-router 0.2.0\n";
                return 0;
            }
            if (option == "--check-config") {
                if (check)
                    throw std::invalid_argument("duplicate argument");
                check = true;
                continue;
            }
            if (option != "--bootstrap-input")
                throw std::invalid_argument("unknown argument");
            if (++i == argc)
                throw std::invalid_argument("missing argument value");
            if (!bootstrap_path.empty())
                throw std::invalid_argument("duplicate argument");
            bootstrap_path = argv[i];
        }
        if (bootstrap_path.empty())
            throw std::invalid_argument("--bootstrap-input is required");
        auto bootstrap = xgc2::xrpc::loadBootstrapInput(bootstrap_path);
        const auto application = bootstrap.application_json();
        if (!application)
            throw std::invalid_argument("native router application configuration required");
        if (application->size() > router::max_config_bytes)
            throw std::invalid_argument("native router configuration exceeds 64 KiB");
        auto config = router::parse_config(router::parse_json(std::string(*application)));
        if (check) {
            std::cout << "configuration valid\n";
            return 0;
        }
        xgc2::xrpc::RuntimePolicyOptions options;
        options.capabilities.push_back("diagnostics");
        options.defaults = {{"MAX_REQUEST_BYTES", "65536"},
                            {"MAX_RESPONSE_BYTES", "262144"},
                            {"HOST_MAX_CONNECTIONS", "16"},
                            {"HOST_MAX_IN_FLIGHT", "16"}};
        options.default_source = "vrpn-router";
        options.ceilings = {{"MAX_REQUEST_BYTES", 65536},
                            {"MAX_RESPONSE_BYTES", 262144},
                            {"HOST_MAX_CONNECTIONS", 16},
                            {"HOST_MAX_IN_FLIGHT", 16}};
        for (char** item = environ; *item; ++item) {
            const std::string entry(*item);
            const auto equals = entry.find('=');
            if (equals != std::string::npos)
                options.environment.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
        }
        return router::run(std::move(config), bootstrap, xgc2::xrpc::resolve_runtime_policy(options));
    } catch (const std::exception& error) {
        std::cerr << "xgc2-vrpn-router: " << error.what() << '\n';
        return 1;
    }
}
